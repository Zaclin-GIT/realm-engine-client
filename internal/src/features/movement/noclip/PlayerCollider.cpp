#include "pch-il2cpp.h"
#include "PlayerCollider.h"
#include "Il2CppResolver.h"
#include "RuntimeOffsets.h"

#include <cstring>

namespace PlayerCollider {
namespace {

constexpr uint32_t kOffCollisionMultiplierFallback = 0x780;
constexpr size_t kMaxObjectPropertiesTargets = 3;
constexpr size_t kMaxEntityCandidates = 2;

void* g_lastPlayer = nullptr;
void* g_lastProperties[kMaxObjectPropertiesTargets]{};
uint32_t g_collisionMultiplierOffset = kOffCollisionMultiplierFallback;

struct EntityCandidate {
    void* ptr = nullptr;
};

bool IsPlausiblePointer(const void* ptr)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    return address > 0x10000ULL && address < 0x7FFFFFFFFFFFULL;
}

FieldInfo* FindFieldOnHierarchy(Il2CppClass* klass, const char* fieldName)
{
    for (Il2CppClass* current = klass; current; current = il2cpp_class_get_parent(current)) {
        FieldInfo* field = il2cpp_class_get_field_from_name(current, fieldName);
        if (field) return field;
    }
    return nullptr;
}

bool TryGetObjectClass(void* object, Il2CppClass*& outClass)
{
    outClass = nullptr;
    if (!Resolver::Protection::IsValidIl2CppObject(object)) return false;
    return Resolver::Protection::safe_call([&]() {
        outClass = il2cpp_object_get_class(reinterpret_cast<Il2CppObject*>(object));
    }) && outClass != nullptr;
}

bool ClassHierarchyHas(Il2CppClass* klass, const char* expectedName)
{
    if (!klass || !expectedName) return false;
    bool matched = false;
    Resolver::Protection::safe_call([&]() {
        for (Il2CppClass* current = klass; current; current = il2cpp_class_get_parent(current)) {
            const char* name = il2cpp_class_get_name(current);
            if (name && std::strcmp(name, expectedName) == 0) {
                matched = true;
                break;
            }
        }
    });
    return matched;
}

bool ResolveFieldOffset(Il2CppClass* klass, const char* fieldName, uint32_t fallback, uint32_t& outOffset, bool& outFromMetadata)
{
    outOffset = fallback;
    outFromMetadata = false;
    if (!klass || !fieldName) return false;

    FieldInfo* field = nullptr;
    if (!Resolver::Protection::safe_call([&]() {
        field = FindFieldOnHierarchy(klass, fieldName);
    }) || !field) {
        return false;
    }

    return Resolver::Protection::safe_call([&]() {
        outOffset = static_cast<uint32_t>(il2cpp_field_get_offset(field));
        outFromMetadata = true;
    });
}

void* ReadPointerRef(void* basePtr, uint32_t offset)
{
    if (!basePtr || offset == 0) return nullptr;
    void* ptr = nullptr;
    __try {
        ptr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(basePtr) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ptr = nullptr;
    }
    return IsPlausiblePointer(ptr) ? ptr : nullptr;
}

void* ReadObjectPropertiesRef(void* entityPtr, uint32_t offset)
{
    return ReadPointerRef(entityPtr, offset);
}

bool ResolveCollisionMultiplierOffset(void* properties)
{
    Il2CppClass* propertiesClass = nullptr;
    if (!TryGetObjectClass(properties, propertiesClass) || !ClassHierarchyHas(propertiesClass, "ObjectProperties"))
        return false;

    uint32_t offset = kOffCollisionMultiplierFallback;
    bool fromMetadata = false;
    ResolveFieldOffset(propertiesClass, "collisionRadiusMultiplier", kOffCollisionMultiplierFallback, offset, fromMetadata);
    (void)fromMetadata;
    g_collisionMultiplierOffset = offset;
    return true;
}

uint32_t ResolveObjectPropertiesOffset(Il2CppClass* entityClass, const ObjectPropertiesTarget& target)
{
    const char* fieldName = nullptr;
    if (std::strcmp(target.label, "base") == 0) fieldName = "OBAKMCCDBJA";
    else if (std::strcmp(target.label, "map-object") == 0) fieldName = "KKENJFFDMPO";
    else if (std::strcmp(target.label, "player-collision") == 0) fieldName = "GGBCADDBAPN";

    uint32_t offset = target.offset;
    bool fromMetadata = false;
    if (fieldName)
        ResolveFieldOffset(entityClass, fieldName, target.offset, offset, fromMetadata);
    return offset;
}

void* ResolveViewDestroyEntity(void* localPlayer)
{
    Il2CppClass* localClass = nullptr;
    uint32_t viewHandlerOffset = RuntimeOffsets::KJ_ViewHandler;
    bool fromMetadata = false;
    if (TryGetObjectClass(localPlayer, localClass))
        ResolveFieldOffset(localClass, "MPGOFIHIDML", RuntimeOffsets::KJ_ViewHandler, viewHandlerOffset, fromMetadata);

    void* viewHandler = ReadPointerRef(localPlayer, viewHandlerOffset);
    Il2CppClass* viewClass = nullptr;
    uint32_t destroyEntityOffset = RuntimeOffsets::VH_DestroyEntity;
    fromMetadata = false;
    if (TryGetObjectClass(viewHandler, viewClass))
        ResolveFieldOffset(viewClass, "destroyEntity", RuntimeOffsets::VH_DestroyEntity, destroyEntityOffset, fromMetadata);

    return ReadPointerRef(viewHandler, destroyEntityOffset);
}

bool TargetsChanged(void* const* properties, size_t propertyCount)
{
    for (size_t i = 0; i < kMaxObjectPropertiesTargets; ++i) {
        if (g_lastProperties[i] != (i < propertyCount ? properties[i] : nullptr)) return true;
    }
    return false;
}

void RememberTargets(void* const* properties, size_t propertyCount)
{
    for (size_t i = 0; i < kMaxObjectPropertiesTargets; ++i)
        g_lastProperties[i] = i < propertyCount ? properties[i] : nullptr;
}

void ClearRememberedTargets()
{
    for (void*& properties : g_lastProperties)
        properties = nullptr;
}

bool AddObjectPropertiesTarget(void** properties, size_t& propertyCount, void* candidate)
{
    if (!candidate || propertyCount >= kMaxObjectPropertiesTargets) return false;
    for (size_t i = 0; i < propertyCount; ++i) {
        if (properties[i] == candidate) return false;
    }
    properties[propertyCount++] = candidate;
    return true;
}

size_t CollectPlayerObjectProperties(void* entity, const ObjectPropertiesTarget* targets, size_t targetCount, void** outProperties, size_t outCapacity)
{
    if (!entity || !targets || outCapacity == 0) return 0;

    Il2CppClass* entityClass = nullptr;
    if (!TryGetObjectClass(entity, entityClass) || !ClassHierarchyHas(entityClass, "FKALGHJIADI"))
        return 0;

    size_t propertyCount = 0;
    const size_t count = targetCount < outCapacity ? targetCount : outCapacity;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t offset = ResolveObjectPropertiesOffset(entityClass, targets[i]);
        void* properties = ReadObjectPropertiesRef(entity, offset);
        if (!ResolveCollisionMultiplierOffset(properties)) continue;
        AddObjectPropertiesTarget(outProperties, propertyCount, properties);
    }
    return propertyCount;
}

bool ApplyPropertiesMultiplier(void* properties, const char* reason, UpdateLogFn logFn)
{
    if (!properties) return false;

    bool updated = false;
    __try {
        float& multiplier = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(properties) + g_collisionMultiplierOffset);
        updated = ApplyMultiplier(multiplier, properties, reason, logFn);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return updated;
}

} // namespace

bool ApplyEntityMultiplier(void* entityPtr,
    uint32_t primaryObjectPropertiesOffset,
    uint32_t secondaryObjectPropertiesOffset,
    uint32_t collisionMultiplierOffset,
    const char* reason,
    UpdateLogFn logFn)
{
    const ObjectPropertiesTarget targets[] = {
        { "primary", primaryObjectPropertiesOffset },
        { "secondary", secondaryObjectPropertiesOffset },
    };
    return ApplyEntityMultiplierTargets(entityPtr, targets, 2, collisionMultiplierOffset, reason, logFn);
}

bool ApplyEntityMultiplierTargets(void* entityPtr,
    const ObjectPropertiesTarget* targets,
    size_t targetCount,
    uint32_t collisionMultiplierOffset,
    const char* reason,
    UpdateLogFn logFn)
{
    if (!entityPtr || !targets || targetCount == 0) return false;

    bool updated = false;
    void* visited[kMaxObjectPropertiesTargets]{};
    size_t visitedCount = 0;
    const size_t count = targetCount < kMaxObjectPropertiesTargets ? targetCount : kMaxObjectPropertiesTargets;
    for (size_t i = 0; i < count; ++i) {
        void* properties = ReadObjectPropertiesRef(entityPtr, targets[i].offset);
        if (!properties) continue;

        bool duplicate = false;
        for (size_t seen = 0; seen < visitedCount; ++seen) {
            if (visited[seen] == properties) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        visited[visitedCount++] = properties;

        __try {
            float& multiplier = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(properties) + collisionMultiplierOffset);
            updated = ApplyMultiplier(multiplier, properties, reason, logFn) || updated;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    return updated;
}

void Tick(void* player)
{
    if (!player) {
        if (g_lastPlayer) ResetScene();
        return;
    }

    const ObjectPropertiesTarget targets[] = {
        { "base", RuntimeOffsets::ObjProps },
        { "map-object", RuntimeOffsets::MoObjectProps },
        { "player-collision", RuntimeOffsets::PlayerCollisionProps },
    };

    EntityCandidate entities[kMaxEntityCandidates] = {
        { player },
        { ResolveViewDestroyEntity(player) },
    };
    if (entities[1].ptr == entities[0].ptr) entities[1].ptr = nullptr;

    void* properties[kMaxObjectPropertiesTargets]{};
    size_t propertyCount = 0;
    for (const EntityCandidate& entity : entities) {
        void* entityProperties[kMaxObjectPropertiesTargets]{};
        const size_t entityPropertyCount = CollectPlayerObjectProperties(entity.ptr, targets, 3, entityProperties, kMaxObjectPropertiesTargets);
        for (size_t i = 0; i < entityPropertyCount; ++i)
            AddObjectPropertiesTarget(properties, propertyCount, entityProperties[i]);
    }

    if (propertyCount == 0)
        return;

    const bool changedObject = player != g_lastPlayer || TargetsChanged(properties, propertyCount);
    g_lastPlayer = player;
    RememberTargets(properties, propertyCount);

    const char* reason = changedObject ? "player-or-scene-change" : "value-restored";
    for (size_t i = 0; i < propertyCount; ++i)
        ApplyPropertiesMultiplier(properties[i], reason, nullptr);
}

void ResetScene()
{
    g_lastPlayer = nullptr;
    ClearRememberedTargets();
}

void ResetStateForTest()
{
    g_lastPlayer = nullptr;
    ClearRememberedTargets();
}

} // namespace PlayerCollider
