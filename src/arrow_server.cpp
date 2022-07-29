/**
 * @file arrow_server.cpp
 * @author Ashot Vardanian
 * @date 2022-07-18
 *
 * @brief An server implementing Apache Arrow Flight RPC protocol.
 *
 * Links:
 * https://arrow.apache.org/cookbook/cpp/flight.html
 */

#include <arrow/buffer.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/flight/client.h>
#include <arrow/flight/server.h>
#include <arrow/pretty_print.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>

#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <vector>
#include <iostream>

using namespace arrow::flight;
using namespace arrow;

// To later implement a C-ABI stable binding:
// https://arrow.apache.org/docs/format/CDataInterface.html#example-use-case
// void ukv_export_arrow(struct ArrowSchema*, struct ArrowArray*);

class UKVService : public arrow::flight::FlightServerBase {
  public:
    const arrow::flight::ActionType kActionDropDataset {"drop_dataset", "Delete a dataset."};

    explicit UKVService(std::shared_ptr<arrow::fs::FileSystem> root) : root_(std::move(root)) {}

    arrow::Status ListFlights(arrow::flight::ServerCallContext const&,
                              arrow::flight::Criteria const*,
                              std::unique_ptr<arrow::flight::FlightListing>* listings) override {
        arrow::fs::FileSelector selector;
        selector.base_dir = "/";
        ARROW_ASSIGN_OR_RAISE(auto listing, root_->GetFileInfo(selector));

        std::vector<arrow::flight::FlightInfo> flights;
        for (const auto& file_info : listing) {
            if (!file_info.IsFile() || file_info.extension() != "parquet")
                continue;
            ARROW_ASSIGN_OR_RAISE(auto info, MakeFlightInfo(file_info));
            flights.push_back(std::move(info));
        }

        *listings =
            std::unique_ptr<arrow::flight::FlightListing>(new arrow::flight::SimpleFlightListing(std::move(flights)));
        return arrow::Status::OK();
    }

    arrow::Status GetFlightInfo(arrow::flight::ServerCallContext const&,
                                arrow::flight::FlightDescriptor const& descriptor,
                                std::unique_ptr<arrow::flight::FlightInfo>* info) override {
        ARROW_ASSIGN_OR_RAISE(auto file_info, FileInfoFromDescriptor(descriptor));
        ARROW_ASSIGN_OR_RAISE(auto flight_info, MakeFlightInfo(file_info));
        *info = std::unique_ptr<arrow::flight::FlightInfo>(new arrow::flight::FlightInfo(std::move(flight_info)));
        return arrow::Status::OK();
    }

    arrow::Status GetSchema(arrow::flight::ServerCallContext const& context,
                            arrow::flight::FlightDescriptor const& request,
                            std::unique_ptr<arrow::flight::SchemaResult>* schema) override {
        return arrow::Status::OK();
    }

    arrow::Status DoPut(arrow::flight::ServerCallContext const&,
                        std::unique_ptr<arrow::flight::FlightMessageReader> reader,
                        std::unique_ptr<arrow::flight::FlightMetadataWriter>) override {

        // ARROW_ASSIGN_OR_RAISE(arrow::Result<std::shared_ptr<arrow::Schema>> schema, reader->GetSchema());
        // ARROW_ASSIGN_OR_RAISE(arrow::Result<arrow::flight::FlightStreamChunk> chunk, reader->Next());

        ARROW_ASSIGN_OR_RAISE(auto file_info, FileInfoFromDescriptor(reader->descriptor()));
        ARROW_ASSIGN_OR_RAISE(auto sink, root_->OpenOutputStream(file_info.path()));
        ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Table> table, reader->ToTable());

        ARROW_RETURN_NOT_OK(
            parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, /*chunk_size=*/65536));
        return arrow::Status::OK();
    }

    arrow::Status DoGet(arrow::flight::ServerCallContext const&,
                        arrow::flight::Ticket const& request,
                        std::unique_ptr<arrow::flight::FlightDataStream>* stream) override {
        ARROW_ASSIGN_OR_RAISE(auto input, root_->OpenInputFile(request.ticket));
        std::unique_ptr<parquet::arrow::FileReader> reader;
        ARROW_RETURN_NOT_OK(parquet::arrow::OpenFile(std::move(input), arrow::default_memory_pool(), &reader));

        std::shared_ptr<arrow::Table> table;
        ARROW_RETURN_NOT_OK(reader->ReadTable(&table));
        // Note that we can't directly pass TableBatchReader to
        // RecordBatchStream because TableBatchReader keeps a non-owning
        // reference to the underlying Table, which would then get freed
        // when we exit this function
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        arrow::TableBatchReader batch_reader(*table);
        ARROW_ASSIGN_OR_RAISE(batches, batch_reader.ToRecordBatches());

        ARROW_ASSIGN_OR_RAISE(auto owning_reader, arrow::RecordBatchReader::Make(std::move(batches), table->schema()));
        *stream = std::unique_ptr<arrow::flight::FlightDataStream>(new arrow::flight::RecordBatchStream(owning_reader));

        return arrow::Status::OK();
    }

    arrow::Status ListActions(arrow::flight::ServerCallContext const&,
                              std::vector<arrow::flight::ActionType>* actions) override {
        *actions = {kActionDropDataset};
        return arrow::Status::OK();
    }

    arrow::Status DoAction(arrow::flight::ServerCallContext const&,
                           arrow::flight::Action const& action,
                           std::unique_ptr<arrow::flight::ResultStream>* result) override {
        if (action.type == kActionDropDataset.type) {
            *result = std::unique_ptr<arrow::flight::ResultStream>(new arrow::flight::SimpleResultStream({}));
            return DoActionDropDataset(action.body->ToString());
        }
        return arrow::Status::NotImplemented("Unknown action type: ", action.type);
    }

  private:
    arrow::Result<arrow::flight::FlightInfo> MakeFlightInfo(arrow::fs::FileInfo const& file_info) {
        ARROW_ASSIGN_OR_RAISE(auto input, root_->OpenInputFile(file_info));
        std::unique_ptr<parquet::arrow::FileReader> reader;
        ARROW_RETURN_NOT_OK(parquet::arrow::OpenFile(std::move(input), arrow::default_memory_pool(), &reader));

        std::shared_ptr<arrow::Schema> schema;
        ARROW_RETURN_NOT_OK(reader->GetSchema(&schema));

        auto descriptor = arrow::flight::FlightDescriptor::Path({file_info.base_name()});

        arrow::flight::FlightEndpoint endpoint;
        endpoint.ticket.ticket = file_info.base_name();
        arrow::flight::Location location;
        ARROW_ASSIGN_OR_RAISE(location, arrow::flight::Location::ForGrpcTcp("localhost", port()));
        endpoint.locations.push_back(location);

        int64_t total_records = 0; // reader->parquet_reader()->metadata()->num_rows();
        int64_t total_bytes = file_info.size();

        return arrow::flight::FlightInfo::Make(*schema, descriptor, {endpoint}, total_records, total_bytes);
    }

    arrow::Result<arrow::fs::FileInfo> FileInfoFromDescriptor(arrow::flight::FlightDescriptor const& descriptor) {
        if (descriptor.type != arrow::flight::FlightDescriptor::PATH) {
            return arrow::Status::Invalid("Must provide PATH-type FlightDescriptor");
        }
        else if (descriptor.path.size() != 1) {
            return arrow::Status::Invalid("Must provide PATH-type FlightDescriptor with one path component");
        }
        return root_->GetFileInfo(descriptor.path[0]);
    }

    arrow::Status DoActionDropDataset(std::string const& key) { return root_->DeleteFile(key); }

    std::shared_ptr<arrow::fs::FileSystem> root_;
};

arrow::Status run_server() {
    auto fs = std::make_shared<arrow::fs::LocalFileSystem>();
    ARROW_RETURN_NOT_OK(fs->CreateDir("./flight_datasets/"));
    ARROW_RETURN_NOT_OK(fs->DeleteDirContents("./flight_datasets/"));
    auto root = std::make_shared<arrow::fs::SubTreeFileSystem>("./flight_datasets/", fs);

    arrow::flight::Location server_location;
    ARROW_ASSIGN_OR_RAISE(server_location, arrow::flight::Location::ForGrpcTcp("0.0.0.0", 38709));

    arrow::flight::FlightServerOptions options(server_location);
    auto server = std::unique_ptr<arrow::flight::FlightServerBase>(new UKVService(std::move(root)));
    ARROW_RETURN_NOT_OK(server->Init(options));
    std::cout << "Listening on port " << server->port() << std::endl;
    return server->Serve();
}

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    return run_server().ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
