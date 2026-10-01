/**
 * @file AngelScriptEngineHotReload.cpp
 * @brief Module hot reload with state carry-over for attached entity scripts
 *
 * AngelScriptEngine::HotReloadModule() recompiles a module from its source
 * file (read once, into a staging module that becomes the module on commit)
 * and HotReloadModuleFromSource() from new in-memory source (modules built by
 * CompileScriptFromString()); both re-attach every entity script of it, applying the hot-reload
 * state rules R1-R8 documented on the method: same-name, same-type fields of
 * carryable types keep their values, everything else keeps the new
 * constructor's value and is reported. Shared by the real and the stub (no
 * AngelScript) builds; the stub cannot reload.
 */

#include "AngelScriptEngine.h"
#include "../../Utils/LogMacros.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

std::string AngelScriptEngine::GetModuleFilePath(const std::string& moduleName) const
{
    auto it = m_moduleFilePaths.find(moduleName);
    return it != m_moduleFilePaths.end() ? it->second : std::string{};
}

std::vector<EntityID> AngelScriptEngine::GetEntitiesForModule(const std::string& moduleName) const
{
    std::vector<EntityID> result;
    for (const auto& [entity, instance] : m_entityScripts)
    {
        if (instance.moduleName == moduleName)
        {
            result.push_back(entity);
        }
    }
    return result;
}

#ifdef SPARK_ANGELSCRIPT_SUPPORT

namespace
{
    /// Record @p note once (many instances of one class report the same field).
    void AddNote(std::vector<std::string>& notes, std::string note)
    {
        if (std::find(notes.begin(), notes.end(), note) == notes.end())
        {
            notes.push_back(std::move(note));
        }
    }
} // namespace

std::vector<AngelScriptEngine::FieldSnapshot> AngelScriptEngine::CaptureFields(asIScriptObject* object) const
{
    std::vector<FieldSnapshot> fields;
    if (!object)
    {
        return fields;
    }

    const int stringTypeId = m_engine->GetTypeIdByDecl("string");
    const asUINT count = object->GetPropertyCount();
    fields.reserve(count);
    for (asUINT i = 0; i < count; ++i)
    {
        const int typeId = object->GetPropertyTypeId(i);
        const char* typeDecl = m_engine->GetTypeDeclaration(typeId, true);

        FieldSnapshot field;
        field.name = object->GetPropertyName(i);
        field.typeDecl = typeDecl ? typeDecl : "";
        const void* address = object->GetAddressOfProperty(i);

        // R3/R4: only self-contained values are carried. A handle or reference
        // object would point into the old module's objects.
        size_t size = 0;
        if (!address || (typeId & asTYPEID_OBJHANDLE) != 0)
        {
            size = 0;
        }
        else if ((typeId & asTYPEID_MASK_OBJECT) == 0)
        {
            // Primitives and enums (enum ids are stable by declaration, not by number).
            size = static_cast<size_t>(std::max(0, m_engine->GetSizeOfPrimitiveType(typeId)));
        }
        else if (typeId == stringTypeId)
        {
            field.carry = FieldCarry::String;
            field.text = *static_cast<const std::string*>(address);
        }
        else if (const asITypeInfo* type = m_engine->GetTypeInfoById(typeId))
        {
            const asQWORD flags = type->GetFlags();
            if ((flags & asOBJ_VALUE) != 0 && (flags & asOBJ_POD) != 0)
            {
                size = type->GetSize();
            }
        }

        if (size > 0)
        {
            field.carry = FieldCarry::Bytes;
            const auto* bytes = static_cast<const unsigned char*>(address);
            field.bytes.assign(bytes, bytes + size);
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

void AngelScriptEngine::RestoreFields(asIScriptObject* object, const std::vector<FieldSnapshot>& fields,
                                      const std::string& location)
{
    HotReloadReport& report = m_lastHotReloadReport;
    std::vector<bool> matched(fields.size(), false);

    const asUINT count = object->GetPropertyCount();
    for (asUINT i = 0; i < count; ++i)
    {
        const std::string name = object->GetPropertyName(i);
        const auto old = std::find_if(fields.begin(), fields.end(),
                                      [&name](const FieldSnapshot& field) { return field.name == name; });
        if (old == fields.end())
        {
            ++report.defaulted; // R5: new field
            continue;
        }
        matched[static_cast<size_t>(old - fields.begin())] = true;

        const char* typeDecl = m_engine->GetTypeDeclaration(object->GetPropertyTypeId(i), true);
        const std::string newDecl = typeDecl ? typeDecl : "";
        if (old->typeDecl != newDecl)
        {
            ++report.dropped; // R5: retyped
            AddNote(report.notes, std::format("{}.{}: retyped from {} to {}, constructor value kept", location, name,
                                              old->typeDecl, newDecl));
            continue;
        }

        void* address = object->GetAddressOfProperty(i);
        if (old->carry == FieldCarry::NotCarried || !address)
        {
            ++report.dropped; // R4
            AddNote(report.notes,
                    std::format("{}.{}: {} is not carried across reload (handle or reference type), constructor "
                                "value kept",
                                location, name, newDecl));
            continue;
        }

        if (old->carry == FieldCarry::String)
        {
            *static_cast<std::string*>(address) = old->text;
        }
        else
        {
            std::memcpy(address, old->bytes.data(), old->bytes.size());
        }
        ++report.carried; // R2
    }

    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (!matched[i])
        {
            ++report.dropped; // R5: removed
            AddNote(report.notes, location + "." + fields[i].name + ": removed");
        }
    }
}

bool AngelScriptEngine::HasScriptClass(const std::string& moduleName, const std::string& className) const
{
    const auto it = m_modules.find(moduleName);
    return it != m_modules.end() && it->second && it->second->GetTypeInfoByName(className.c_str()) != nullptr;
}

bool AngelScriptEngine::HotReloadModule(const std::string& moduleName)
{
    m_lastHotReloadReport = HotReloadReport{};

    auto fileIt = m_moduleFilePaths.find(moduleName);
    if (fileIt == m_moduleFilePaths.end())
    {
        SetLastError("No file path recorded for module '" + moduleName + "'. Cannot hot-reload.");
        SPARK_LOG_ERROR(Spark::LogCategory::Scripting, "%s", m_lastError.c_str());
        return false;
    }
    // Copied: the map may rehash while this runs.
    const std::string filePath = fileIt->second;
    return StageAndCommitReload(moduleName, "'" + filePath + "'", [&filePath](CScriptBuilder& builder)
                                { return builder.AddSectionFromFile(filePath.c_str()); });
}

bool AngelScriptEngine::HotReloadModuleFromSource(const std::string& moduleName, const std::string& source)
{
    m_lastHotReloadReport = HotReloadReport{};

    if (m_modules.find(moduleName) == m_modules.end())
    {
        SetLastError("Module '" + moduleName + "' is not compiled. Cannot hot-reload it from source.");
        SPARK_LOG_ERROR(Spark::LogCategory::Scripting, "%s", m_lastError.c_str());
        return false;
    }
    if (source.empty())
    {
        SetLastError("Hot-reload rejected: the new source of module '" + moduleName + "' is empty.");
        SPARK_LOG_ERROR(Spark::LogCategory::Scripting, "%s", m_lastError.c_str());
        return false;
    }

    // The section is named after the module, as CompileScriptFromString() names it.
    return StageAndCommitReload(moduleName, "module '" + moduleName + "' source",
                                [&moduleName, &source](CScriptBuilder& builder) {
                                    return builder.AddSectionFromMemory(moduleName.c_str(), source.c_str(),
                                                                        static_cast<unsigned int>(source.size()));
                                });
}

bool AngelScriptEngine::StageAndCommitReload(const std::string& moduleName, const std::string& origin,
                                             const std::function<int(CScriptBuilder&)>& addSection)
{
    // 1. Snapshot every entity script of this module before anything changes.
    struct SavedBinding
    {
        EntityID entity;
        std::string className;
        bool started;
        std::vector<FieldSnapshot> fields;
    };
    std::vector<SavedBinding> bindings;
    for (const auto& [entity, instance] : m_entityScripts)
    {
        if (instance.moduleName == moduleName)
        {
            bindings.push_back({entity, instance.className, instance.started, CaptureFields(instance.object)});
        }
    }

    // 2. R1: build the new source into a staging module BEFORE touching any
    //    live instance. The common case is a file saved mid-edit with a syntax
    //    error; detaching first would wipe every running script of the module
    //    with no way back.
    //
    //    The source (and its #includes) is read exactly once, here. The staged
    //    module is the one that gets committed below: compiling the file a
    //    second time after the detach would re-read a file that an editor may
    //    be rewriting (truncate-then-write save, watcher firing mid-write), and
    //    a failure there would leave every entity of the module without a script.
    const std::string stagingModule = moduleName + "$hotreload_stage";
    m_firstCompileError.clear();
    CScriptBuilder builder;
    const bool staged = builder.StartNewModule(m_engine, stagingModule.c_str()) >= 0 && addSection(builder) >= 0 &&
                        builder.BuildModule() >= 0;
    asIScriptModule* stage = m_engine->GetModule(stagingModule.c_str());
    if (!staged || !stage)
    {
        if (stage)
        {
            stage->Discard();
        }
        SetLastError("Hot-reload rejected: recompilation of " + origin + " failed (" + m_firstCompileError +
                     "); live scripts left intact.");
        SPARK_LOG_ERROR(Spark::LogCategory::Scripting, "%s", m_lastError.c_str());
        return false;
    }

    // 3. Commit. Nothing below reads the file or can fail to compile: detach the
    //    old instances, discard the old module (Discard() removes its name from
    //    the engine at once) and rename the staged module to the canonical name.
    for (const auto& binding : bindings)
    {
        DetachScript(binding.entity);
    }
    if (asIScriptModule* previous = m_engine->GetModule(moduleName.c_str()))
    {
        previous->Discard();
    }
    stage->SetName(moduleName.c_str());
    m_modules[moduleName] = stage;
    RecordModuleContexts(builder, moduleName);

    // 4. Re-attach (constructor runs, Start() does not: R6) and carry state over (R2-R5, R7).
    HotReloadReport& report = m_lastHotReloadReport;
    for (const auto& binding : bindings)
    {
        const std::string location = moduleName + "::" + binding.className;
        if (!AttachScript(binding.entity, binding.className, moduleName))
        {
            ++report.failedAttaches; // R8
            AddNote(report.notes, location + ": re-attach to entity " +
                                      std::to_string(static_cast<uint32_t>(binding.entity)) +
                                      " failed, entity left without a script (" + m_lastError + ")");
            continue;
        }
        ++report.instances;
        if (auto* instance = GetScriptInstance(binding.entity); instance != nullptr)
        {
            instance->started = binding.started;
        }
        RestoreFields(GetScriptInstance(binding.entity)->object, binding.fields, location);
    }

    SPARK_LOG_INFO(Spark::LogCategory::Scripting,
                   "Hot-reloaded module '%s': %zu instance(s) re-attached, %zu failed; fields carried %zu, "
                   "defaulted %zu, dropped %zu.",
                   moduleName.c_str(), report.instances, report.failedAttaches, report.carried, report.defaulted,
                   report.dropped);
    for (const auto& note : report.notes)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scripting, "Hot-reload: %s", note.c_str());
    }
    return report.failedAttaches == 0;
}

#else // !SPARK_ANGELSCRIPT_SUPPORT

bool AngelScriptEngine::HotReloadModule(const std::string& moduleName)
{
    m_lastHotReloadReport = HotReloadReport{};
    SPARK_LOG_WARN(Spark::LogCategory::Scripting, "Cannot hot-reload module '%s': AngelScript support not compiled in.",
                   moduleName.c_str());
    SetLastError("AngelScript support not available.");
    return false;
}

bool AngelScriptEngine::HotReloadModuleFromSource(const std::string& moduleName, const std::string& /*source*/)
{
    return HotReloadModule(moduleName);
}

bool AngelScriptEngine::HasScriptClass(const std::string& /*moduleName*/, const std::string& /*className*/) const
{
    return false;
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
