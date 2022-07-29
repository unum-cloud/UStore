#include <arrow/api.h>
#include <arrow/dataset/table.h>
#include <arrow/dataset/dataset.h>
#include <arrow/dataset/discovery.h>
#include <arrow/dataset/scanner.h>

class CollectionTable : public arrow::Table {

    /// @brief Return a column by index
    std::shared_ptr<arrow::ChunkedArray> column(int i) const override;
    arrow::Result<std::shared_ptr<arrow::Table>> RemoveColumn(int i) const override;

    /// @brief Return vector of all columns for table
    std::vector<std::shared_ptr<arrow::ChunkedArray>> const& columns() const override;

    /// @brief Construct a zero-copy slice of the table with the
    /// indicated offset and length
    ///
    /// @param[in] offset the index of the first row in the constructed
    /// slice
    /// @param[in] length the number of rows of the slice. If there are not enough
    /// rows in the table, the length will be adjusted accordingly
    ///
    /// @return a new object wrapped in std::shared_ptr<Table>
    std::shared_ptr<arrow::Table> Slice(int64_t offset, int64_t length) const override;

    /// @brief Add column to the table, producing a new Table
    arrow::Result<std::shared_ptr<arrow::Table>> AddColumn( //
        int i,
        std::shared_ptr<arrow::Field> field_arg,
        std::shared_ptr<arrow::ChunkedArray> column) const override;

    /// @brief Replace a column in the table, producing a new Table
    arrow::Result<std::shared_ptr<arrow::Table>> SetColumn( //
        int i,
        std::shared_ptr<arrow::Field> field_arg,
        std::shared_ptr<arrow::ChunkedArray> column) const override;

    /// @brief Replace schema key-value metadata with new metadata
    /// @since 0.5.0
    ///
    /// @param[in] metadata new KeyValueMetadata
    /// @return new Table
    std::shared_ptr<arrow::Table> ReplaceSchemaMetadata( //
        std::shared_ptr<arrow::KeyValueMetadata> const& metadata) const override;

    /// @brief Flatten the table, producing a new Table.  Any column with a
    /// struct type will be flattened into multiple columns
    ///
    /// @param[in] pool The pool for buffer allocations, if any
    arrow::Result<std::shared_ptr<arrow::Table>> Flatten( //
        arrow::MemoryPool* pool = arrow::default_memory_pool()) const override;

    /// @brief Perform cheap validation checks to determine obvious inconsistencies
    /// within the table's schema and internal data.
    ///
    /// This is O(k*m) where k is the total number of field descendents,
    /// and m is the number of chunks.
    ///
    /// @return Status
    arrow::Status Validate() const override;

    /// @brief Perform extensive validation checks to determine inconsistencies
    /// within the table's schema and internal data.
    ///
    /// This is O(k*n) where k is the total number of field descendents,
    /// and n is the number of rows.
    ///
    /// @return Status
    arrow::Status ValidateFull() const override;
};

int main(int argc, char** argv) {
    return EXIT_SUCCESS;
}