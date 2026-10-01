/**
 * @file SceneJSONReader.h
 * @brief Decoder for the editor's JSON scene document (.sparkscene, .json, .scenejson)
 *
 * SceneSerializer::LoadJSON reads a scene file and hands its bytes to
 * DecodeSceneJSONDocument, which owns the whole document grammar: the bounded JSON
 * parser (depth 128, 250,000 nodes), the header fields and the N / N-1 version window
 * (OD-03), objects and hierarchy, schema-tagged component payloads, environment,
 * camera, asset references, the declared-count check and SceneFile::Validate. The
 * decoder lives in its own translation unit so the SEC-120 fuzz target links the shipped
 * reader without the serializer's file I/O and logging.
 *
 * Thread affinity: any thread; the functions keep no shared state.
 * Ownership: the caller owns every argument. DecodeSceneJSONDocument replaces
 * @p outScene only when the whole document decodes and validates.
 * Allocation: the parsed document tree and the decoded scene, bounded by the parser's
 * depth and node limits and by the caller's file-size limit.
 */

#pragma once

#include "SceneSerializer.h"

#include <string>
#include <string_view>

namespace SparkEditor
{

    /**
     * @brief Whether @p value is well-formed UTF-8 (no overlong forms, surrogates, or code
     *        points above U+10FFFF). The writer and the reader share this definition.
     */
    bool IsValidSceneUTF8(std::string_view value);

    /**
     * @brief Run SceneFile::Validate and append every message to @p result.warnings.
     * @return true when the scene is valid.
     */
    bool AppendSceneValidation(const SceneFile& scene, SerializationResult& result);

    /**
     * @brief Decode one complete JSON scene document.
     *
     * On success @p outScene holds the decoded scene, result.success is true and
     * result.bytesProcessed is @p content's size. On failure @p outScene is untouched,
     * result.success stays false and result.errorMessage names the first violation;
     * result.warnings collects validation messages and the in-memory N-1 migration note.
     *
     * @param content The file's bytes.
     * @param outScene Receives the decoded scene on success.
     * @param result Receives the verdict, error, and warnings.
     * @return true when the document decoded and validated.
     */
    bool DecodeSceneJSONDocument(const std::string& content, SceneFile& outScene, SerializationResult& result);

} // namespace SparkEditor
