/**
 * @file media.h
 * @author Ashot Vardanian
 * @date 27 Jun 2022
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ustore/db.h"

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

/**
 * @brief Formats describing contents of collections.
 * The low-level interface
 *
 * Many of the numerical values are set to their RFC proposal numbers.
 * https://en.wikipedia.org/wiki/List_of_RFCs
 */
typedef enum {
    ustore_format_field_default_k = 0,
    ustore_format_graph_k = 1,
    ustore_format_doc_k = 2,
    ustore_format_table_k = 3,

    // Flexible dynamically-typed document formats
    // https://github.com/msgpack/msgpack/blob/master/spec.md#type-system
    ustore_format_msgpack_k = 11,
    ustore_format_bson_k = 12,
    ustore_format_ubjson_k = 13,
    ustore_field_json_k = 7159,
    ustore_format_cbor_k = 7049,

    // Patches and modifiers to documents
    // https://stackoverflow.com/a/64882070/2766161
    ustore_format_json_patch_k = 6902,       // RFC
    ustore_format_json_merge_patch_k = 7386, // RFC

    ustore_format_csv_k = 4180,
    ustore_format_arrow_k = 14,
    ustore_format_parquet_k = 15,

    // Generic text-based formats, that  generally come in long chunks
    // would benefit from compression and may require full-text search.
    ustore_format_text_k = 20,
    ustore_format_text_xml_k = 3470,
    ustore_format_text_html_k = 1866,

    // Image formats
    ustore_format_img_jpeg200_k = 3745, // RFC
    ustore_format_img_jpeg_k = 1314,    // RFC
    ustore_format_img_png_k = 2083,     // RFC
    ustore_format_img_gif_k = 51,
    ustore_format_img_webp_k = 52,

} ustore_format_field_type_t;

#ifdef __cplusplus
} /* end extern "C" */
#endif
