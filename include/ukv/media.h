/**
 * @file media.h
 * @author Ashot Vardanian
 * @date 27 Jun 2022
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ukv/db.h"

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
    ukv_format_field_default_k = 0,
    ukv_format_graph_k = 1,
    ukv_format_doc_k = 2,
    ukv_format_table_k = 3,

    // Flexible dynamically-typed document formats
    // https://github.com/msgpack/msgpack/blob/master/spec.md#type-system
    ukv_format_msgpack_k = 11,
    ukv_format_bson_k = 12,
    ukv_format_ubjson_k = 13,
    ukv_field_json_k = 7159,
    ukv_format_cbor_k = 7049,

    // Patches and modifiers to documents
    // https://stackoverflow.com/a/64882070/2766161
    ukv_format_json_patch_k = 6902,       // RFC
    ukv_format_json_merge_patch_k = 7386, // RFC

    ukv_format_csv_k = 4180,
    ukv_format_arrow_k = 14,
    ukv_format_parquet_k = 15,

    // Generic text-based formats, that  generally come in long chunks
    // would benefit from compression and may require full-text search.
    ukv_format_text_k = 20,
    ukv_format_text_xml_k = 3470,
    ukv_format_text_html_k = 1866,

    // Image formats
    ukv_format_img_jpeg200_k = 3745, // RFC
    ukv_format_img_jpeg_k = 1314,    // RFC
    ukv_format_img_png_k = 2083,     // RFC
    ukv_format_img_gif_k = 51,
    ukv_format_img_webp_k = 52,

} ukv_format_field_type_t;

#ifdef __cplusplus
} /* end extern "C" */
#endif
