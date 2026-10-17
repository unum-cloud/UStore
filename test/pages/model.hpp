/**
 *  @file test/pages/model.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The naive reference model of a page that `test/pages/page_fold.cpp` checks the fold against, and that
 *      `test/units.cpp` checks every engine's pages calls against.
 *
 *  The model keeps a page as a map from keys to elements with vectors of children, and applies every edit by the
 *  forgiving rules `page_fold.hpp` names, with nothing shared with the templates beyond the key and kind types. It
 *  renders XML and text as `markup.hpp` documents them, again without its code.
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <algorithm> // `std::find`, `std::min`
#include <map>       // `std::map`
#include <string>    // `std::string`
#include <utility>   // `std::pair`
#include <vector>    // `std::vector`

#include "ustore/pages/element_tree.hpp" // `element_key_t`, `element_kind_t`
#include "ustore/pages/page_edits.hpp"   // `modification_t`

namespace unum::ustore::test {

using namespace unum::ustore::pages;

/** The 32 hexadecimal digits of @p key. */
inline std::string hex(element_key_t const& key) {
    static char const digits[] = "0123456789abcdef";
    std::string result;
    for (std::uint8_t byte : key.bytes) result += digits[byte >> 4], result += digits[byte & 15];
    return result;
}

/** One element of a fragment in preorder, with its depth below the fragment's root, starting at one. */
struct node_spec_t {
    element_key_t key;
    element_kind_t kind = element_kind_t::element_k;
    std::string name, text;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::size_t depth = 1;
};

struct model_element_t {
    element_kind_t kind = element_kind_t::element_k;
    std::string name, text;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<element_key_t> children;
    element_key_t parent;
};

/** One edit as the model reads it, with the fragment it inserts or imports spelled out. */
struct model_edit_t {
    modification_t modification = modification_t::import_k;
    std::uint64_t stamp = 0;
    std::uint32_t sequence = 0;
    element_key_t element, parent, anchor;
    std::uint64_t text_start = 0, text_length = 0;
    std::string field, value;
    std::vector<node_spec_t> fragment;
};

/** A page as a map of live elements, edited by the rules `page_fold.hpp` names, without any of its code. */
struct model_page_t {
    bool exists = false;
    element_key_t root;
    std::map<element_key_t, model_element_t> elements;

    bool live(element_key_t const& key) const { return elements.count(key) != 0; }

    bool contains(element_key_t const& ancestor, element_key_t descendant) const {
        for (;;) {
            if (descendant == ancestor) return true;
            if (descendant == root) return false;
            descendant = elements.at(descendant).parent;
        }
    }

    void erase(element_key_t const& key) {
        for (element_key_t const& child : std::vector<element_key_t>(elements.at(key).children)) erase(child);
        elements.erase(key);
    }

    void unlink(element_key_t const& key) {
        auto& siblings = elements.at(elements.at(key).parent).children;
        siblings.erase(std::find(siblings.begin(), siblings.end(), key));
    }

    /** Where an element lands among @p parent's children after @p anchor, front for a Nil or foreign anchor. */
    std::size_t landing(element_key_t const& parent, element_key_t const& anchor) const {
        auto const& siblings = elements.at(parent).children;
        auto found = std::find(siblings.begin(), siblings.end(), anchor);
        return found == siblings.end() ? 0 : std::size_t(found - siblings.begin()) + 1;
    }

    void graft(element_key_t const& parent, std::size_t position, std::vector<node_spec_t> const& fragment) {
        std::vector<element_key_t> open {parent};
        for (node_spec_t const& spec : fragment) {
            open.resize(spec.depth);
            model_element_t element;
            element.kind = spec.kind, element.name = spec.name, element.text = spec.text;
            element.attributes = spec.attributes, element.parent = open.back();
            if (spec.depth == 1)
                elements[open.back()].children.insert(elements[open.back()].children.begin() + position++, spec.key);
            else elements[open.back()].children.push_back(spec.key);
            elements[spec.key] = element;
            open.push_back(spec.key);
        }
    }

    void apply(model_edit_t const& edit) {
        switch (edit.modification) {
        case modification_t::import_k:
            exists = true, root = edit.element;
            elements.clear();
            elements[root] = model_element_t {};
            graft(root, 0, edit.fragment);
            return;
        case modification_t::remove_k:
            if (edit.element.is_nil()) {
                exists = false;
                elements.clear();
                return;
            }
            if (!exists || !live(edit.element)) return;
            if (edit.element == root) {
                for (element_key_t const& child : std::vector<element_key_t>(elements[root].children)) erase(child);
                elements[root].children.clear(), elements[root].text.clear(), elements[root].attributes.clear();
                return;
            }
            unlink(edit.element);
            erase(edit.element);
            return;
        case modification_t::insert_k: {
            if (!exists) return;
            element_key_t const parent = edit.parent.is_nil() ? root : edit.parent;
            if (!live(parent) || elements[parent].kind != element_kind_t::element_k) return;
            for (node_spec_t const& spec : edit.fragment)
                if (live(spec.key)) return;
            graft(parent, landing(parent, edit.anchor), edit.fragment);
            return;
        }
        case modification_t::move_k: {
            if (!exists || !live(edit.element) || edit.element == root || edit.anchor == edit.element) return;
            element_key_t const parent = edit.parent.is_nil() ? root : edit.parent;
            if (!live(parent) || elements[parent].kind != element_kind_t::element_k || contains(edit.element, parent))
                return;
            unlink(edit.element);
            std::size_t const position = landing(parent, edit.anchor);
            elements[parent].children.insert(elements[parent].children.begin() + position, edit.element);
            elements[edit.element].parent = parent;
            return;
        }
        case modification_t::splice_k: {
            if (!exists || !live(edit.element)) return;
            std::string& text = elements[edit.element].text;
            std::size_t const start = std::size_t(std::min<std::uint64_t>(edit.text_start, text.size()));
            std::size_t const length = std::size_t(std::min<std::uint64_t>(edit.text_length, text.size() - start));
            text.replace(start, length, edit.value);
            return;
        }
        case modification_t::set_attributes_k: {
            if (!exists || !live(edit.element) || elements[edit.element].kind != element_kind_t::element_k) return;
            auto& attributes = elements[edit.element].attributes;
            for (auto& [name, value] : attributes)
                if (name == edit.field) {
                    value = edit.value;
                    return;
                }
            attributes.emplace_back(edit.field, edit.value);
            return;
        }
        case modification_t::remove_attributes_k: {
            if (!exists || !live(edit.element) || elements[edit.element].kind != element_kind_t::element_k) return;
            auto& attributes = elements[edit.element].attributes;
            for (auto at = attributes.begin(); at != attributes.end(); ++at)
                if (at->first == edit.field) {
                    attributes.erase(at);
                    return;
                }
            return;
        }
        }
    }

    /** Every live element in preorder, in the format of @c dump_tree. */
    std::string dump() const {
        if (!exists) return "absent\n";
        std::string result;
        dump_element(root, 0, result);
        return result;
    }

    void dump_element(element_key_t const& key, std::size_t depth, std::string& result) const {
        model_element_t const& element = elements.at(key);
        result += std::string(depth * 2, ' ') + hex(key) + " " + std::to_string(int(element.kind)) + " <" +
                  element.name + "> [" + element.text + "]";
        for (auto const& [name, value] : element.attributes) result += " " + name + "=" + value;
        result += "\n";
        for (element_key_t const& child : element.children) dump_element(child, depth + 1, result);
    }

    /** The XML of the subtree of @p key, the root rendering as its content alone. */
    std::string render_xml(element_key_t const& key) const {
        std::string result;
        render_xml_into(key, result);
        return result;
    }

    /** The text of every element and text element under @p key in document order, comments dropped. */
    std::string render_text(element_key_t const& key) const {
        model_element_t const& element = elements.at(key);
        if (element.kind == element_kind_t::comment_k) return {};
        std::string result = element.text;
        for (element_key_t const& child : element.children) result += render_text(child);
        return result;
    }

    static std::string escaped(std::string const& bytes, bool inside_attribute) {
        std::string result;
        for (char character : bytes)
            if (character == '&') result += "&amp;";
            else if (character == '<') result += "&lt;";
            else if (character == '>' && !inside_attribute) result += "&gt;";
            else if (character == '"' && inside_attribute) result += "&quot;";
            else result += character;
        return result;
    }

    void render_xml_into(element_key_t const& key, std::string& result) const {
        model_element_t const& element = elements.at(key);
        if (element.kind == element_kind_t::text_k) {
            result += escaped(element.text, false);
            return;
        }
        if (element.kind == element_kind_t::comment_k) {
            result += "<!--" + element.text + "-->";
            return;
        }
        bool const named = !element.name.empty();
        bool const content = !element.text.empty() || !element.children.empty();
        if (named) {
            result += "<" + element.name;
            for (auto const& [name, value] : element.attributes)
                result += " " + name + "=\"" + escaped(value, true) + "\"";
            result += content ? ">" : "/>";
        }
        result += escaped(element.text, false);
        for (element_key_t const& child : element.children) render_xml_into(child, result);
        if (named && content) result += "</" + element.name + ">";
    }
};

} // namespace unum::ustore::test
