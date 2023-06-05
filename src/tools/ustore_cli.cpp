/**
 * @file cli.cpp
 * @brief CLI tool for UStore.
 */

#include <iostream>            // `std::cout`, `std::cerr`
#include <regex>               // `std::regex`, `std::regex_token_iterator`

#include <fmt/format.h>        // `fmt::format`, `fmt::print`

#include <clipp.h>             // `clipp::parse`
#include <readline/readline.h> // `readline`
#include <readline/history.h>  // `add_history`

#include "ustore/cpp/db.hpp"   // `database_t`
#include "dataset.h"           // `import`, `export`

using namespace unum::ustore;
using namespace unum;

#pragma region - Helpers
static constexpr char const* red_k = "\033[31m";
static constexpr char const* green_k = "\033[32m";
static constexpr char const* yellow_k = "\x1B[33m";
static constexpr char const* reset_k = "\033[0m";

inline std::string remove_quotes(std::string str) {
    if (str[0] == '\"' && str[str.size() - 1] == '\"') {
        str.erase(0, 1);
        str.erase(str.size() - 1);
    }
    return str;
}

template <typename... args_at>
inline void print(char const* color, std::string message, args_at&&... args) {
    if (sizeof...(args) != 0)
        message = fmt::format(message, std::forward<args_at>(args)...);
    fmt::print("{}{}{}\n", color, message, reset_k);
}
#pragma endregion - Helpers

#pragma region - Collection
inline void collection_create(database_t& db, std::string const& name) {
    auto maybe_collection = db.find_or_create(name.c_str());
    if (maybe_collection)
        print(green_k, "Collection '{}' created", name);
    else
        print(red_k, "Failed to create collection '{}'", name);
}

inline void collection_drop(database_t& db, std::string const& name) {
    auto status = db.drop(name);
    if (status)
        print(green_k, "Collection '{}' dropped", name);
    else
        print(red_k, "Failed to drop collection '{}'", name);
}

void collection_list(database_t& db) {
    auto context = context_t {db, nullptr};
    auto collections = context.collections();

    if (!collections) {
        print(red_k, "Failed to list collections");
        return;
    }

    while (!collections->names.is_end()) {
        print(yellow_k, "{}", *collections->names);
        ++collections->names;
    }
}
#pragma endregion - Collection

#pragma region - Snapshot
inline void snapshot_create(database_t& db) {
    auto snapshot = db.snapshot();
    if (snapshot)
        print(green_k, "Snapshot created");
    else
        print(red_k, "Failed to create snapshot");
}

inline void snapshot_export(database_t& db, std::string const& path) {
    auto context = context_t {db, nullptr};
    auto status = context.export_to(path.c_str());
    if (status)
        print(green_k, "Snapshot exported");
    else
        print(red_k, "Failed to export snapshot");
}

void snapshot_drop(database_t& db, ustore_snapshot_t const& id) {
    auto status = db.drop_snapshot(id);
    if (status)
        print(green_k, "Snapshot dropped");
    else
        print(red_k, "Failed to drop snapshot");
}

void snapshot_list(database_t& db) {
    auto context = context_t {db, nullptr};
    auto snapshots = context.snapshots().throw_or_release();

    for (auto it = snapshots.begin(); it != snapshots.end(); ++it)
        print(yellow_k, "{}", *it);
}
#pragma endregion - Snapshot

#pragma region - Import/Export
void docs_import(database_t& db,
                 std::string const& collection_name,
                 std::string const& input_file,
                 std::string const& id_field,
                 std::size_t max_batch_size) {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.find(collection_name);

    ustore_docs_import_t docs {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_pattern = input_file.c_str(),
        .max_batch_size = max_batch_size,
        .id_field = id_field.c_str(),
    };
    ustore_docs_import(&docs);

    if (status)
        print(green_k, "Successfully imported");
    else
        print(red_k, "Failed to import: {}", status.message());
}

void docs_export(database_t& db,
                 std::string const& collection_name,
                 std::string const& output_ext,
                 std::size_t max_batch_size) {
    status_t status;
    arena_t arena(db);
    ustore_collection_t collection = db.find(collection_name);

    ustore_docs_export_t docs {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .options = ustore_options_default_k,
        .collection = collection,
        .paths_extension = output_ext.c_str(),
        .max_batch_size = max_batch_size,
    };
    ustore_docs_export(&docs);

    if (status)
        print(green_k, "Successfully exported");
    else
        print(red_k, "Failed to export: {}", status.message());
}
#pragma endregion - Import / Export

#pragma region - Interface

// List of CLI arguments
struct cli_args_t {
    bool help = false;
    std::string url;

    std::string action;
    std::string db_object;
    std::string col_name;
    ustore_snapshot_t snap_id;

    std::string id_field;
    std::string input_file;
    std::string output_ext;
    std::string export_path;
    std::size_t memory_limit;
};

// CLI arguments parser
bool parse_cli_args(int argc, char* argv[], cli_args_t& arg) {
    using namespace clipp;

    auto collection =
        (option("collection").set(arg.db_object, std::string("collection")) &
         ((required("create").set(arg.action, std::string("create")) & required("--name") &
           value("collection name", arg.col_name)) |
          (required("drop").set(arg.action, std::string("drop")) & required("--name") &
           value("collection name", arg.col_name)) |
          required("list").set(arg.action, std::string("list")) |
          ((required("import").set(arg.action, std::string("import")) &
            (required("--input") & value("input", arg.input_file)).doc("Input file path") &
            (required("--id") & value("id field", arg.id_field)).doc("The field which data will use as key(s)")) |
           (required("export").set(arg.action, std::string("export")) &
            (required("--output") & value("output", arg.output_ext)).doc("Output file path"))) &
              ((required("--mlimit") & value("memory limit", arg.memory_limit))
                   .doc("Size of available RAM for a specific operation in bytes"),
               (option("--name") & value("collection name", arg.col_name)))));

    auto snapshot = (option("snapshot").set(arg.db_object, std::string("snapshot")) &
                     ((required("create").set(arg.action, std::string("create"))) |
                      (required("export").set(arg.action, std::string("export")) & value("path", arg.export_path)) |
                      (required("drop").set(arg.action, std::string("drop")) & value("snapshot id", arg.snap_id)) |
                      (required("list").set(arg.action, std::string("list")))));

    auto cli = ((required("--url") & value("URL", arg.url)).doc("Server URL"),
                (collection | snapshot),
                option("-h", "--help").set(arg.help).doc("Print this help information on this tool and exit"));

    if (!parse(argc, argv, cli)) {
        std::cerr << make_man_page(cli, argv[0]);
        return false;
    }

    if (arg.help)
        std::cout << make_man_page(cli, argv[0]);

    return true;
}

// CLI commands executor
bool execute(cli_args_t& arg, database_t& db) {
    if (arg.db_object == "collection") {
        if (arg.action == "create")
            collection_create(db, arg.col_name);
        else if (arg.action == "drop")
            collection_drop(db, arg.col_name);
        else if (arg.action == "list")
            collection_list(db);
        else if (arg.action == "import")
            docs_import(db, arg.col_name, arg.input_file, arg.id_field, arg.memory_limit);
        else if (arg.action == "export")
            docs_export(db, arg.col_name, arg.output_ext, arg.memory_limit);
        else
            print(red_k, "Invalid collection action {}", arg.action);
        return true;
    }
    else if (arg.db_object == "snapshot") {
        if (arg.action == "create")
            snapshot_create(db);
        else if (arg.action == "export")
            snapshot_export(db, arg.export_path);
        else if (arg.action == "drop")
            snapshot_drop(db, arg.snap_id);
        else if (arg.action == "list")
            snapshot_list(db);
        else
            print(red_k, "Invalid snapshot action {}", arg.action);
        return true;
    }

    return false;
}

#pragma region - Interactive CLI
bool parse_collection_args(cli_args_t& cli_arg, std::vector<std::string>& cmd_line) {
    cli_arg.action = cmd_line[1];
    if (cli_arg.action == "create") {
        if (cmd_line.size() != 3) {
            print(red_k, "Invalid input");
            return false;
        }
        cli_arg.col_name = remove_quotes(cmd_line[2]);
    }
    else if (cli_arg.action == "drop") {
        if (cmd_line.size() != 3) {
            print(red_k, "Invalid input");
            return false;
        }
        cli_arg.col_name = remove_quotes(cmd_line[2]);
    }
    else if (cli_arg.action == "list") {
        if (cmd_line.size() != 2) {
            print(red_k, "Invalid input");
            return false;
        }
    }
    else {
        print(red_k, "Invalid collection action {}", cli_arg.action);
        return false;
    }

    return true;
}

bool parse_snapshot_args(cli_args_t& cli_arg, std::vector<std::string>& cmd_line) {
    cli_arg.action = cmd_line[1];
    if (cli_arg.action == "create") {
        if (cmd_line.size() != 2) {
            print(red_k, "Invalid input");
            return false;
        }
    }
    else if (cli_arg.action == "export") {
        if (cmd_line.size() != 3) {
            print(red_k, "Invalid input");
            return false;
        }
    }
    else if (cli_arg.action == "drop") {
        if (cmd_line.size() != 3) {
            print(red_k, "Invalid input");
            return false;
        }
    }
    else if (cli_arg.action == "list") {
        if (cmd_line.size() != 2) {
            print(red_k, "Invalid input");
            return false;
        }
    }
    else {
        print(red_k, "Invalid snapshot action {}", cli_arg.action);
        return false;
    }

    return true;
}

bool parse_import_args(cli_args_t& cli_arg, std::vector<std::string>& cmd_line) {
    if (cmd_line.size() != 9 && cmd_line.size() != 7) {
        print(red_k, "Invalid input");
        return false;
    }

    std::string argument = cmd_line[1];
    if (argument == "--input")
        cli_arg.input_file = cmd_line[2];
    else {
        print(red_k, "Invalid list argument {}", argument);
        return false;
    }

    argument = cmd_line[3];
    if (argument == "--id")
        cli_arg.id_field = cmd_line[4];
    else {
        print(red_k, "Invalid list argument {}", argument);
        return false;
    }

    argument = cmd_line[5];
    if (argument == "--mlimit")
        cli_arg.memory_limit = std::stoi(cmd_line[6]);
    else {
        print(red_k, "Invalid list argument {}", argument);
        return false;
    }

    if (cmd_line.size() == 9) {
        argument = cmd_line[7];
        if (argument == "--collection")
            cli_arg.col_name = cmd_line[8];
        else {
            print(red_k, "Invalid list argument {}", argument);
            return false;
        }
    }
    else {
        cli_arg.col_name = "";
    }

    return true;
}

bool parse_export_args(cli_args_t& cli_arg, std::vector<std::string>& cmd_line) {
    if (cmd_line.size() != 7 && cmd_line.size() != 5) {
        print(red_k, "Invalid input");
        return false;
    }

    std::string argument = cmd_line[1];
    if (argument == "--output")
        cli_arg.output_ext = cmd_line[2];
    else {
        print(red_k, "Invalid list argument {}", argument);
        return false;
    }

    argument = cmd_line[3];
    if (argument == "--mlimit")
        cli_arg.memory_limit = std::stoi(cmd_line[4]);
    else {
        print(red_k, "Invalid list argument {}", argument);
        return false;
    }

    if (cmd_line.size() == 7) {
        argument = cmd_line[5];
        if (argument == "--collection")
            cli_arg.col_name = cmd_line[6];
        else {
            print(red_k, "Invalid list argument {}", argument);
            return false;
        }
    }
    else
        cli_arg.col_name = "";

    return true;
}

// The main loop of interactive CLI tool
void interactive_cli(database_t& db) {
    cli_args_t args;
    std::string input;
    std::vector<std::string> cmd_line;
    std::regex const reg_exp(" +(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)");
    std::regex_token_iterator<std::string::iterator> const end_tokens;

    while (true) {
        cmd_line.clear();
        input = readline(">>> ");
        add_history(input.c_str());

        std::regex_token_iterator<std::string::iterator> it(input.begin(), input.end(), reg_exp, -1);
        while (it != end_tokens)
            cmd_line.emplace_back(*it++);

        if (!cmd_line[0].size())
            cmd_line.erase(cmd_line.begin());

        args.db_object = cmd_line[0];
        if (args.db_object == "exit")
            break;

        if (args.db_object == "clear") {
            system("clear");
            continue;
        }

        bool status = false;
        if (args.db_object == "collection")
            status = parse_collection_args(args, cmd_line);
        else if (args.db_object == "snapshot")
            status = parse_snapshot_args(args, cmd_line);
        else if (args.db_object == "import")
            status = parse_import_args(args, cmd_line);
        else if (args.db_object == "export")
            status = parse_export_args(args, cmd_line);
        else
            print(red_k, "Unknown command {}", args.db_object);

        if (status)
            execute(args, db);
    }
}
#pragma endregion - Interactive CLI

#pragma endregion - Interface

int main(int argc, char* argv[]) {
    cli_args_t arg;
    bool result = parse_cli_args(argc, argv, arg);

    if (!result || arg.help)
        return !result | !arg.help;

    database_t db;
    status_t status = db.open(arg.url.c_str());

    if (!status) {
        print(red_k, status.message());
        return 1;
    }

    if (execute(arg, db))
        return 0;

    interactive_cli(db);
    return 0;
}
