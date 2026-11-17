/**
 *  @file test/pages/element_tree.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Unit tests of `ustore/pages/element_tree.hpp` and `ustore/pages/markup.hpp`: the dense form's round trips
 *      and subtree addressing, refusals of malformed pages, XML and text import and render, and XPath spans.
 *
 *  The suite links no engine, only SmashTable and StringZilla, so it checks the templates apart
 *  from any call routed through the C ABI.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <iterator>    // `std::size`
#include <memory>      // `std::unique_ptr`, `std::align`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <vector>      // `std::vector`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/capabilities.h>  // `sz_capabilities_compiled_cpu_mask`, `sz_capabilities_enabled_cpu`

#include "ustore/pages/element_tree.hpp" // `element_tree`, `page_view_t`
#include "ustore/pages/markup.hpp"       // `import_xml`, `render_xml`, `find_xpath`

#include "harness.hpp" // `expect_true`, `expect_equal`, `expect_status`, `run_test_case`

namespace unum::ustore::test {

using namespace unum::ustore::pages;

/** The widest StringZilla kernels this build compiled. */
using strings_kit_t = utilities::strings_ladder<sz_capabilities_compiled_cpu_mask()>::strings_kit_t;

/** The StringZilla capabilities this machine runs among those the build compiled, which the engines
 *  compile for, while @c strings_kit_t keeps to a tier the build's own flags guarantee. */
sz_capability_t strings_capabilities() noexcept {
    sz_capability_t capabilities = sz_cap_serial_k;
    if (sz_capabilities_enabled_cpu(&capabilities) != sz_success_k) capabilities = sz_cap_serial_k;
    return capabilities;
}

#pragma region Arenas

/** A bump arena handing out blocks that live until it is destroyed, as engines' arenas do. */
struct test_arena_t {
    std::vector<std::unique_ptr<std::byte[]>> blocks;
};

/** The allocator over a @c test_arena_t, freeing nothing until the arena goes. */
template <typename value_type_>
struct test_allocator {
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;

    test_arena_t* arena = nullptr;

    test_allocator(test_arena_t* owner = nullptr) noexcept : arena(owner) {}
    template <typename other_type_>
    test_allocator(test_allocator<other_type_> const& other) noexcept : arena(other.arena) {}

    value_type_* allocate(std::size_t count) noexcept {
        std::size_t const bytes = count * sizeof(value_type_) + alignof(std::max_align_t);
        auto block = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[bytes]);
        if (!block) return nullptr;
        void* pointer = block.get();
        std::size_t space = bytes;
        pointer = std::align(alignof(value_type_), count * sizeof(value_type_), pointer, space);
        arena->blocks.push_back(std::move(block));
        return static_cast<value_type_*>(pointer);
    }
    void deallocate(value_type_*, std::size_t) noexcept {}

    template <typename other_type_>
    bool operator==(test_allocator<other_type_> const& other) const noexcept {
        return arena == other.arena;
    }
};

using bytes_allocator_t = test_allocator<byte_t>;
using tree_t = element_tree<bytes_allocator_t>;
using chars_t = st::vector<char, test_allocator<char>>;

/** The characters @p chars holds. */
static std::string_view view_of(chars_t const& chars) noexcept { return {chars.begin(), chars.end()}; }

#pragma endregion Arenas

#pragma region Random Trees

/** Hex of a key, for dumps. */
std::string hex(element_key_t const& key) {
    static char const digits[] = "0123456789abcdef";
    std::string result;
    for (std::uint8_t byte : key.bytes) result += digits[byte >> 4], result += digits[byte & 15];
    return result;
}

/** Every element of @p tree in preorder: depth, key, placement, kind, name, text and attributes, one line each. */
std::string dump(tree_t const& tree) {
    std::string result;
    element_index_t const root = tree.root();
    for (element_index_t at = root; at != no_element_k; at = tree.next_in_preorder(at, root)) {
        std::size_t depth = 0;
        for (element_index_t up = at; up != root; up = tree[up].parent) ++depth;
        result += std::string(depth * 2, ' ');
        result += hex(tree[at].key) + "@" + std::to_string(tree[at].placement.stamp) + "." +
                  std::to_string(tree[at].placement.sequence) + " ";
        result += std::to_string(int(tree[at].kind)) + " <" + std::string(tree.name(at)) + "> [" +
                  std::string(tree.text(at)) + "]";
        for (element_index_t attribute = tree[at].first_attribute; attribute != no_element_k;
             attribute = tree.attribute(attribute).next)
            result += " " + std::string(tree.string(tree.attribute(attribute).name)) + "=" +
                      std::string(tree.string(tree.attribute(attribute).value));
        result += "\n";
    }
    return result;
}

/** A random string over an alphabet rich in bytes that markup escapes. */
std::string random_text(split_mix_generator_t& random, std::size_t longest) {
    static constexpr std::string_view alphabet = "ab xy&<>\"'\n;#";
    std::string result(random.below(longest + 1), ' ');
    for (char& character : result) character = alphabet[random.below(alphabet.size())];
    return result;
}

/** A random name an XML tag or attribute may carry. */
std::string random_name(split_mix_generator_t& random) {
    static constexpr std::string_view names[] = {"p", "div", "span", "x:item", "row", "cell"};
    return std::string(names[random.below(std::size(names))]);
}

/** How far a random tree grows: linked only, or edited afterwards with splices, attributes,
 *  removals and moves. */
enum class tree_growth_t : bool { links_only_k, then_edits_k };

/** Grows a random tree of up to @p count elements, then edits it when @p growth asks for that. */
status_t grow_random(tree_t& tree, split_mix_generator_t& random, std::size_t count, tree_growth_t growth) {
    std::uint32_t ordinal = 0;
    if (status_t status = tree.reset(mint_key(1, 0, ordinal++), {1, 0}); status.failed()) return status;
    std::vector<element_index_t> elements {tree.root()};
    for (std::size_t index = 0; index != count; ++index) {
        element_index_t const parent = elements[random.below(elements.size())];
        element_kind_t const kind = element_kind_t(random.below(3));
        element_index_t after = no_element_k;
        if (random.below(2))
            for (element_index_t child = tree[parent].first_child; child != no_element_k;
                 child = tree[child].next_sibling)
                if (random.below(2)) after = child;
        placement_t const placement {random.below(9) + 1, std::uint32_t(random.below(3))};
        auto added = tree.link(parent, after, mint_key(1, 0, ordinal++), kind,
                               kind == element_kind_t::element_k ? std::string_view(random_name(random)) : "",
                               random_text(random, 6), placement);
        if (!added) return added.status();
        if (kind != element_kind_t::element_k) continue;
        elements.push_back(*added);
        for (std::size_t attribute = random.below(3); attribute; --attribute)
            if (status_t status = tree.set_attribute(*added, random_name(random), random_text(random, 4));
                status.failed())
                return status;
    }
    if (growth == tree_growth_t::links_only_k) return {};
    for (std::size_t index = 0; index != count / 2; ++index) {
        element_index_t const target = elements[random.below(elements.size())];
        if (tree[target].removed) continue;
        switch (random.below(5)) {
        case 0:
            if (status_t status = tree.splice(target, random.below(8), random.below(4), random_text(random, 3));
                status.failed())
                return status;
            break;
        case 1: tree.remove_attribute(target, random_name(random)); break;
        case 2:
            if (target != tree.root() && random.below(3) == 0) tree.remove(target);
            break;
        case 3: {
            element_index_t const parent = elements[random.below(elements.size())];
            if (tree[parent].removed || target == tree.root() || tree.contains(target, parent)) break;
            tree.unlink(target);
            tree.relink(target, parent, tree.landing(parent, nil_key_k, {99, 0}), {99, 0});
            break;
        }
        default:
            if (status_t status = tree.set_attribute(target, random_name(random), random_text(random, 2));
                status.failed())
                return status;
        }
    }
    return {};
}

#pragma endregion Random Trees

#pragma region Cases

/** Minted keys order as their stamps, sequences and ordinals, and Nil stands apart. */
void test_keys_and_placements() {
    expect_true(nil_key_k.is_nil());
    expect_true(!mint_key(0, 0, 1).is_nil());
    expect_true(mint_key(1, 0, 0) < mint_key(1, 0, 1));
    expect_true(mint_key(1, 9, 9) < mint_key(2, 0, 0));
    expect_true(mint_key(1, 1, 0) < mint_key(1, 2, 0));
    expect_true(mint_key(300, 0, 0) > mint_key(255, 0xFFFFFFFF, 0xFFFFFFFF));
    expect_true(placement_t {1, 5} < placement_t {2, 0});
    expect_true(placement_t {2, 1} > placement_t {2, 0});
}

/** Random trees survive encoding and decoding with every key, placement, string and attribute in place, and encode
 *  to the same bytes again. */
void test_dense_round_trip() {
    split_mix_generator_t random {case_stream_key()};
    for (std::size_t round = 0; round != 200; ++round) {
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        tree_t tree(allocator);
        expect_true(grow_random(tree, random, random.below(40), tree_growth_t::then_edits_k).succeeded());
        auto blob = tree.encode(allocator);
        expect_true(bool(blob));
        if (!blob) continue;
        expect_true(blob->size() == tree.encoded_size());
        auto view = page_view_t::make(*blob);
        expect_true(bool(view));
        if (!view) continue;
        expect_true(view->size() == tree.size());
        expect_true(view->root_key() == tree[tree.root()].key);
        tree_t decoded(allocator);
        expect_status(decoded.decode(*blob), status_code_t::success_k);
        expect_equal(dump(decoded), dump(tree));
        auto again = decoded.encode(allocator);
        expect_true(bool(again) && again->size() == blob->size() &&
                    std::memcmp(again->data(), blob->data(), blob->size()) == 0);
    }
}

/** The directory finds every live key, misses removed ones, and a subtree is the contiguous run of its preorder. */
void test_view_addresses_subtrees() {
    split_mix_generator_t random {case_stream_key()};
    for (std::size_t round = 0; round != 100; ++round) {
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        tree_t tree(allocator);
        expect_true(grow_random(tree, random, 30, tree_growth_t::then_edits_k).succeeded());
        auto blob = tree.encode(allocator);
        auto view = page_view_t::make(*blob);
        expect_true(bool(view));
        if (!view) continue;
        element_index_t const root = tree.root();
        std::uint32_t position = 0;
        for (element_index_t at = root; at != no_element_k; at = tree.next_in_preorder(at, root), ++position) {
            expect_true(view->find(tree[at].key) == position);
            std::uint32_t size = 0;
            for (element_index_t inner = at; inner != no_element_k; inner = tree.next_in_preorder(inner, at)) ++size;
            page_subtree_t const subtree = view->subtree(tree[at].key);
            expect_true(subtree.first == position && subtree.count == size);
            expect_true(view->record(position).kind == tree[at].kind);
            expect_true(view->string(view->record(position).text) == tree.text(at));
        }
        for (element_index_t at = 0; at != tree.capacity(); ++at)
            if (tree[at].removed && tree.find(tree[at].key) == no_element_k)
                expect_true(view->find(tree[at].key) == no_element_k);
        expect_true(view->subtree(mint_key(77, 77, 77)).count == 0);
    }
}

/** Truncated, resized and corrupted pages are refused as malformed, and random byte flips never crash a decode. */
void test_malformed_pages() {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    split_mix_generator_t random {case_stream_key()};
    tree_t tree(allocator);
    expect_true(grow_random(tree, random, 20, tree_growth_t::links_only_k).succeeded());
    auto blob = tree.encode(allocator);
    std::vector<byte_t> bytes(blob->begin(), blob->end());
    tree_t decoded(allocator);

    expect_status(decoded.decode({bytes.data(), 5}), status_code_t::malformed_k);
    expect_status(decoded.decode({bytes.data(), bytes.size() - 1}), status_code_t::malformed_k);
    expect_true(decoded.empty());

    std::vector<byte_t> corrupted = bytes;
    store_u32(corrupted.data(), load_u32(corrupted.data()) + 1);
    expect_status(decoded.decode(corrupted), status_code_t::malformed_k);

    corrupted = bytes;
    store_u32(corrupted.data() + page_header_bytes_k + 32, 1000);
    expect_status(decoded.decode(corrupted), status_code_t::malformed_k);

    corrupted = bytes;
    corrupted[page_header_bytes_k + 28] = byte_t(9);
    expect_status(decoded.decode(corrupted), status_code_t::malformed_k);

    // A second record repeating the first one's key breaks the directory's strict order
    corrupted = bytes;
    std::memcpy(corrupted.data() + page_header_bytes_k + page_record_bytes_k, corrupted.data() + page_header_bytes_k,
                16);
    expect_status(decoded.decode(corrupted), status_code_t::malformed_k);

    for (std::size_t round = 0; round != 2000; ++round) {
        corrupted = bytes;
        for (std::size_t flips = random.below(4) + 1; flips; --flips)
            corrupted[random.below(corrupted.size())] ^= byte_t(1u << random.below(8));
        status_t const status = decoded.decode(corrupted);
        expect_true(status.succeeded() || status.code == status_code_t::malformed_k);
        if (status.succeeded()) expect_true(bool(decoded.encode(allocator)));
    }
}

/** Importing XML keeps elements, attributes, mixed content, comments, CDATA and entities, and renders canonically. */
void test_xml_import_render() {
    struct sample_t {
        std::string_view input;
        std::string_view rendered;
        std::string_view text;
    };
    sample_t const samples[] = {
        {"<a/>", "<a/>", ""},
        {"<a></a>", "<a/>", ""},
        {"<a>hey</a>", "<a>hey</a>", "hey"},
        {"<?xml version=\"1.0\"?><a b='1' c=\"x&quot;y\">t</a>", "<a b=\"1\" c=\"x&quot;y\">t</a>", "t"},
        {"<a>x<b/>y<!--note-->z</a>", "<a>x<b/>y<!--note-->z</a>", "xyz"},
        {"<a>&lt;&amp;&gt;&#65;&#x42;</a>", "<a>&lt;&amp;&gt;AB</a>", "<&>AB"},
        {"<a><![CDATA[<raw & stuff>]]>tail</a>", "<a>&lt;raw &amp; stuff&gt;tail</a>", "<raw & stuff>tail"},
        {"<!DOCTYPE note><r><s:t k=\"v\"/></r>", "<r><s:t k=\"v\"/></r>", ""},
        {"<r>\n  <c>1</c>\n  <c>2</c>\n</r>\n", "<r>\n  <c>1</c>\n  <c>2</c>\n</r>\n", "\n  1\n  2\n\n"},
        {"<r a = \"1\"\n b='&lt;'>x</r>", "<r a=\"1\" b=\"&lt;\">x</r>", "x"},
        {"lead<a/>trail", "lead<a/>trail", "leadtrail"},
        {"<a b='&apos;&#x10FFFF;'>&#9;]]</a>", "<a b=\"&#39;\xF4\x8F\xBF\xBF\">\t]]</a>", "\t]]"},
    };
    for (sample_t const& sample : samples) {
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        tree_t tree(allocator);
        std::uint32_t ordinal = 0;
        auto mint = [&]() noexcept { return mint_key(5, 0, ++ordinal); };
        expect_status(import_xml<strings_kit_t>(sample.input, mint_key(5, 0, 0), {5, 0}, mint, tree),
                      status_code_t::success_k);
        chars_t rendered {test_allocator<char>(&arena)}, text {test_allocator<char>(&arena)};
        expect_status(render_xml<strings_kit_t>(tree, tree.root(), rendered), status_code_t::success_k);
        expect_status(render_text(tree, tree.root(), text), status_code_t::success_k);
        expect_equal(view_of(rendered), sample.rendered);
        expect_equal(view_of(text), sample.text);
        expect_true(tree.size() == ordinal + 1);
    }

    // Leading text is the element's own run, later runs are text elements
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    tree_t tree(allocator);
    std::uint32_t ordinal = 0;
    auto mint = [&]() noexcept { return mint_key(5, 0, ++ordinal); };
    expect_status(import_xml<strings_kit_t>("<p>a<b>c</b>d</p>", mint_key(5, 0, 0), {5, 0}, mint, tree),
                  status_code_t::success_k);
    element_index_t const paragraph = tree.find(mint_key(5, 0, 1));
    expect_true(paragraph != no_element_k && tree.text(paragraph) == "a");
    element_index_t const bold = tree.find(mint_key(5, 0, 2));
    expect_true(bold != no_element_k && tree.text(bold) == "c" && tree.name(bold) == "b");
    element_index_t const tail = tree.find(mint_key(5, 0, 3));
    expect_true(tail != no_element_k && tree[tail].kind == element_kind_t::text_k && tree.text(tail) == "d");

    // Rendering a subtree renders that element with its tags
    chars_t rendered {test_allocator<char>(&arena)};
    expect_status(render_xml<strings_kit_t>(tree, bold, rendered), status_code_t::success_k);
    expect_equal(view_of(rendered), "<b>c</b>");
}

/** Rendering a random tree and importing it renders the same bytes again, and keeps its text. */
void test_xml_round_trip_random() {
    split_mix_generator_t random {case_stream_key()};
    for (std::size_t round = 0; round != 300; ++round) {
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        tree_t tree(allocator);
        expect_true(grow_random(tree, random, random.below(30), tree_growth_t::then_edits_k).succeeded());
        chars_t first {test_allocator<char>(&arena)}, second {test_allocator<char>(&arena)};
        chars_t first_text {test_allocator<char>(&arena)}, second_text {test_allocator<char>(&arena)};
        expect_true(render_xml<strings_kit_t>(tree, tree.root(), first).succeeded());
        expect_true(render_text(tree, tree.root(), first_text).succeeded());
        // Comments may hold `-->` or `--` that the random text spells, which no XML can carry
        if (view_of(first).find("<!--") != std::string_view::npos) {
            bool spoiled = false;
            for (element_index_t at = 0; at != tree.capacity(); ++at)
                spoiled |= !tree[at].removed && tree[at].kind == element_kind_t::comment_k &&
                           tree.text(at).find('>') != std::string_view::npos;
            if (spoiled) continue;
        }
        tree_t imported(allocator);
        std::uint32_t ordinal = 0;
        auto mint = [&]() noexcept { return mint_key(2, 0, ++ordinal); };
        status_t const status = import_xml<strings_kit_t>(view_of(first), mint_key(2, 0, 0), {2, 0}, mint, imported);
        expect_status(status, status_code_t::success_k);
        if (status.failed()) {
            fmt::println("      input: [{}]", view_of(first));
            continue;
        }
        expect_true(render_xml<strings_kit_t>(imported, imported.root(), second).succeeded());
        expect_true(render_text(imported, imported.root(), second_text).succeeded());
        expect_equal(view_of(second), view_of(first));
        // Comments drop out of the text render, but a comment's body never reaches it either way
        expect_equal(view_of(second_text), view_of(first_text));
    }
}

/** Broken markup is refused, an internal DOCTYPE subset is unsupported, and so are HTML, Markdown and JSON. */
void test_markup_refusals() {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    tree_t tree(allocator);
    std::uint32_t ordinal = 0;
    auto mint = [&]() noexcept { return mint_key(3, 0, ++ordinal); };
    element_key_t const root = mint_key(3, 0, 0);
    std::string_view const malformed[] = {
        "<a>",
        "<a></b>",
        "</a>",
        "<a><!-- x</a>",
        "<a><![CDATA[x</a>",
        "<a b=\"1></a>",
        "<a b=1/>",
        "<a b></a>",
        "<a b=\"1\" b=\"2\"/>",
        "<a/",
        "<>",
        "<a></a></a>",
        "<?xml",
        "<a>&unknown;</a>",
        "<a>a & b</a>",
        "<a>&amp</a>",
        "<a b=\"&nbsp;\"/>",
        "<a b=\"<\"/>",
        "<a>&#0;</a>",
        "<a>&#xD800;</a>",
        "<a>&#X41;</a>",
        "<a>&#12a;</a>",
        "<a>x]]>y</a>",
    };
    for (std::string_view input : malformed)
        expect_status(import_xml<strings_kit_t>(input, root, {}, mint, tree), status_code_t::malformed_k);
    expect_status(import_xml<strings_kit_t>("<!DOCTYPE a [<!ENTITY e \"x\">]><a/>", root, {}, mint, tree),
                  status_code_t::unsupported_k);
    for (page_encoding_t encoding : {page_encoding_t::html_k, page_encoding_t::markdown_k, page_encoding_t::json_k}) {
        expect_status(import_markup<strings_kit_t>(encoding, "<a/>", root, {}, mint, tree),
                      status_code_t::unsupported_k);
        expect_status(import_markup<strings_kit_t>(page_encoding_t::xml_k, "<a/>", root, {}, mint, tree),
                      status_code_t::success_k);
        chars_t output {test_allocator<char>(&arena)};
        expect_status(render_markup<strings_kit_t>(tree, tree.root(), encoding, output), status_code_t::unsupported_k);
    }
}

/** Plain text imports as one text element, renders back as itself, and escapes as XML. */
void test_text_import_render() {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    tree_t tree(allocator);
    std::uint32_t ordinal = 0;
    auto mint = [&]() noexcept { return mint_key(4, 0, ++ordinal); };
    expect_status(import_markup<strings_kit_t>(page_encoding_t::text_k, "a < b & c", mint_key(4, 0, 0), {}, mint, tree),
                  status_code_t::success_k);
    chars_t text {test_allocator<char>(&arena)}, xml {test_allocator<char>(&arena)};
    expect_status(render_markup<strings_kit_t>(tree, tree.root(), page_encoding_t::text_k, text),
                  status_code_t::success_k);
    expect_status(render_markup<strings_kit_t>(tree, tree.root(), page_encoding_t::xml_k, xml),
                  status_code_t::success_k);
    expect_equal(view_of(text), "a < b & c");
    expect_equal(view_of(xml), "a &lt; b &amp; c");
    expect_true(tree.size() == 2);
    expect_status(import_markup<strings_kit_t>(page_encoding_t::text_k, "", mint_key(4, 0, 0), {}, mint, tree),
                  status_code_t::success_k);
    expect_true(tree.size() == 1);
}

/** XPath matches over a rendering map back to the elements whose spans start where they do. */
void test_xpath_spans() {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    tree_t tree(allocator);
    std::uint32_t ordinal = 0;
    auto mint = [&]() noexcept { return mint_key(6, 0, ++ordinal); };
    expect_status(import_xml<strings_kit_t>("<doc><b id=\"1\">x</b><c><b id=\"2\"/></c><b id=\"3\">y<i/></b></doc>",
                                            mint_key(6, 0, 0), {}, mint, tree),
                  status_code_t::success_k);
    chars_t rendered {test_allocator<char>(&arena)};
    st::vector<element_span_t, test_allocator<element_span_t>> spans {test_allocator<element_span_t>(&arena)};
    expect_status(render_xml<strings_kit_t>(tree, tree.root(), rendered, spans), status_code_t::success_k);
    expect_true(spans.size() == tree.size());
    for (element_span_t const& span : spans)
        if (tree[span.element].kind == element_kind_t::element_k && !tree.name(span.element).empty()) {
            std::string_view const bytes(rendered.data() + span.begin, span.end - span.begin);
            expect_true(bytes.starts_with("<" + std::string(tree.name(span.element))));
            expect_true(bytes.ends_with(">"));
        }

    std::string_view const paths[] = {"//b", "/doc/b[2]/i"};
    st::vector<sz_match_t, test_allocator<sz_match_t>> matches {test_allocator<sz_match_t>(&arena)};
    expect_status(find_xpath<strings_kit_t>(view_of(rendered), paths, strings_capabilities(), matches),
                  status_code_t::success_k);
    std::vector<std::string> found;
    for (sz_match_t const& match : matches) {
        element_index_t const element = element_starting(std::span<element_span_t const>(spans.begin(), spans.end()),
                                                         match.byte_offset);
        expect_true(element != no_element_k);
        if (element == no_element_k) continue;
        element_index_t const identifier = tree.find_attribute(element, "id");
        found.push_back(std::to_string(match.needle_index) + ":" + std::string(tree.name(element)) + ":" +
                        (identifier == no_element_k ? "" : std::string(tree.string(tree.attribute(identifier).value))));
    }
    std::vector<std::string> const wanted {"0:b:1", "0:b:2", "0:b:3", "1:i:"};
    expect_true(found == wanted);

    std::string_view const broken[] = {"//b[last()]"};
    expect_status(find_xpath<strings_kit_t>(view_of(rendered), broken, strings_capabilities(), matches),
                  status_code_t::invalid_argument_k);
}

/** The SmashTable heap allocator frees every block the tree and the tokenizer took, as the sanitizers check. */
void test_heap_allocator() {
    using heap_t = st::default_allocator<byte_t>;
    heap_t allocator;
    element_tree<heap_t> tree(allocator);
    std::uint32_t ordinal = 0;
    auto mint = [&]() noexcept { return mint_key(8, 0, ++ordinal); };
    expect_status(import_xml<strings_kit_t>("<a x=\"&amp;\">b<c/>&lt;d</a>", mint_key(8, 0, 0), {}, mint, tree),
                  status_code_t::success_k);
    auto blob = tree.encode(allocator);
    expect_true(bool(blob));
    element_tree<heap_t> decoded(allocator);
    expect_status(decoded.decode(*blob), status_code_t::success_k);
    expect_true(decoded.size() == tree.size());
    allocator.deallocate(const_cast<byte_t*>(blob->data()), blob->size());
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore::test;
    start_test_run(argc, argv);
    run_test_case("test_keys_and_placements", test_keys_and_placements);
    run_test_case("test_dense_round_trip", test_dense_round_trip);
    run_test_case("test_view_addresses_subtrees", test_view_addresses_subtrees);
    run_test_case("test_malformed_pages", test_malformed_pages);
    run_test_case("test_xml_import_render", test_xml_import_render);
    run_test_case("test_xml_round_trip_random", test_xml_round_trip_random);
    run_test_case("test_markup_refusals", test_markup_refusals);
    run_test_case("test_text_import_render", test_text_import_render);
    run_test_case("test_xpath_spans", test_xpath_spans);
    run_test_case("test_heap_allocator", test_heap_allocator);
    return finish_test_run();
}
