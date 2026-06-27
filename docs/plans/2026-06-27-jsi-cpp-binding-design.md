# JSI C++ Binding: Fast-Path Property Architecture

**Date:** 2026-06-27
**Target:** Android V8 runtime (`android/runtime/v8/src/native/`), iOS JSC runtime (`iphone/`)
**Status:** Design proposal — not implemented

## Table of Contents

1. [Problem](#problem)
2. [Goal](#goal)
3. [Current Call Path (Android)](#current-call-path-android)
4. [Design: Dual C++/Java Property Store](#design-dual-cjava-property-store)
5. [Files to Create / Modify](#files-to-create--modify)
6. [Implementation Phases](#implementation-phases)
7. [iOS Port](#ios-port)
8. [Performance Projections](#performance-projections)
9. [Risks and Mitigations](#risks-and-mitigations)
10. [Appendix: Glossary](#appendix-glossary)

---

## Problem

Setting a simple property like `label.text = 'hello'` on Android incurs full JNI overhead
every time, even when there are no event listeners and the property only needs storage:

```
V8 setter interceptor
  → Proxy::onPropertyChanged (C++)
    → TypeConverter::jsStringToJavaString     (V8 string → jstring, UTF-16 alloc)
    → TypeConverter::jsValueToJavaObject      (type dispatch + box allocation)
    → proxy->getJavaObject()                  (ReferenceTable lookup)
    → JNI CallVoidMethod(onPropertyChanged)   (boundary crossing)
      → Java KrollProxy.onPropertyChanged()
        → HashMap.put("text", "hello")
        → firePropertyChanged → no listeners → no-op
    → unreferenceJavaObject + DeleteLocalRef × 2
  → setPropertyOnProxy (V8 _properties internal map)
```

**6+ JNI operations for a single string property set.** The overhead compounds during
animated sequences, table view scrolling, or creation dicts with many properties.

---

## Goal

Create a **C++-authoritative property store** inside the existing `titanium::Proxy`
class that:

1. Stores simple property values entirely in C++ — zero JNI on set/get.
2. Batches dirty properties and flushes them to Java in a single JNI call.
3. Skips the Java KrollProxy event-dispatch chain when no listeners exist.
4. Is **opt-in per property** — non-registered properties fall through to the existing path.
5. Is **platform-agnostic** — an iOS port uses the same C++ core with an ObjC flush backend.

---

## Current Call Path (Android)

### Property Set (`label.text = 'hello'`)

```
[JS]     label.text = 'hello'
  ↓
[V8]     NamedPropertySetter callback
  ↓ Proxy.cpp:onPropertyChanged(Local<Name>, Local<Value>, PropertyCallbackInfo<void>)
  ↓ Proxy.cpp:onPropertyChangedForProxy()
  ↓
[C++]    jstring javaProperty = TypeConverter::jsStringToJavaString(isolate, env, property)
[C++]    jobject javaValue = TypeConverter::jsValueToJavaObject(isolate, env, value, &isNew)
[C++]    jobject javaProxy = proxy->getJavaObject()
[C++]    env->CallVoidMethod(javaProxy, JNIUtil::krollProxyOnPropertyChangedMethod, javaProperty, javaValue)
  ↓
[Java]   KrollProxy.onPropertyChanged(String name, Object value)
[Java]   properties.put(name, value)
[Java]   firePropertyChanged(name, oldValue, newValue)
  ↓ (if View proxy)
[Java]   modelListener.propertyChanged(name, oldValue, newValue, proxy)
  ↓ (if TiUIView)
[Java]   view.setText("hello")  // actual Android API call
  ↓
[C++]    env->DeleteLocalRef(javaProperty)
[C++]    if (isNew) env->DeleteLocalRef(javaValue)
[C++]    proxy->unreferenceJavaObject(javaProxy)
  ↓
[C++]    setPropertyOnProxy(isolate, property, value, proxyObject)  // V8 _properties map
```

### Property Get (`console.log(label.text)`)

```
[JS]     console.log(label.text)
  ↓
[V8]     NamedPropertyGetter callback
  ↓ Proxy.cpp:getProperty()
  ↓
[JS]     Proxy.prototype.getProperty  (defined in kroll.js)
  ↓
[C++]    lookup in V8 _properties internal map
  ↓
[JS]     return value
```

(The getter already avoids JNI for simple properties stored in the V8 `_properties` map.)

---

## Design: Dual C++/Java Property Store

### High-Level Architecture

```
┌─────────────────┐     setProperty/getProperty     ┌──────────────┐
│   V8 Isolate     │ ◀─────────────────────────────▶ │  Proxy (C++) │
│  (JS objects)    │                                 │              │
└─────────────────┘                                 │  ┌──────────────────┐
                                                    │  │ FastPropertyStore │
                                                    │  │  map<string,      │
                                                    │  │   Persistent<Val>>│
                                                    │  └──────────────────┘
                                                    │  ┌──────────────────┐
                                                    │  │ dirtyProperties_  │
                                                    │  └──────────────────┘
                                                    └───────┬──────────┘
                                                            │ flush (batched JNI)
                                                            ▼
                                               ┌────────────────────────┐
                                               │  KrollProxy (Java)     │
                                               │  - properties HashMap  │
                                               │  - modelListener       │
                                               └────────┬───────────────┘
                                                        │ View setter
                                                        ▼
                                               ┌────────────────────────┐
                                               │  Android View (Java)   │
                                               └────────────────────────┘
```

### Key Data Structure: `FastPropertyStore`

```cpp
// New file: FastPropertyStore.h

#include <string>
#include <unordered_map>
#include <vector>
#include <v8.h>

namespace titanium {

// Holds property values authoritatively in C++.
// Properties registered as "fast" skip JNI entirely on set/get.
// Non-registered properties fall through to the existing JNI path.
class FastPropertyStore {
public:
    using PropertyMap = std::unordered_map<
        std::string,
        v8::Persistent<v8::Value, v8::CopyablePersistentTraits<v8::Value>>
    >;

    FastPropertyStore() = default;
    ~FastPropertyStore();

    // Lifecycle
    void set(v8::Isolate* isolate, const std::string& name, v8::Local<v8::Value> value);
    v8::Local<v8::Value> get(v8::Isolate* isolate, const std::string& name) const;
    bool has(const std::string& name) const;
    void remove(v8::Isolate* isolate, const std::string& name);
    void clear(v8::Isolate* isolate);

    // Dirty tracking
    void markDirty(const std::string& name);
    const std::vector<std::string>& dirtyProperties() const { return dirtyProperties_; }
    void clearDirty() { dirtyProperties_.clear(); }

    // Bulk export for JNI flush
    // Returns pairs of (propertyName, jobject value) in index order of dirtyProperties_
    void exportDirty(v8::Isolate* isolate, JNIEnv* env,
                     std::vector< std::pair<std::string, jobject> >& out) const;

private:
    PropertyMap map_;
    std::vector<std::string> dirtyProperties_;
};

} // namespace titanium
```

### Modifications to Existing `Proxy` (C++)

#### `Proxy.h` — Additions

```cpp
// In class Proxy, add:
class Proxy : public JavaObject {
    // ... existing members ...

    // === JSI Fast Property Store ===
public:
    // Register a property name as "hot" — its sets/gets skip JNI entirely.
    // Must be called once during proxy type initialization.
    static void registerFastProperty(const std::string& propName);

    // Check if a property is registered as fast.
    static bool isFastProperty(const std::string& propName);

    // Access the C++ property store for this proxy instance.
    FastPropertyStore& fastProperties() { return fastProperties_; }

    // Called from JS idle callback or rAF to push dirty props to Java.
    // Returns true if any properties were flushed.
    bool flushDirtyProperties(JNIEnv* env);

    // Does this proxy have JS listeners for the given event type?
    // Returns false if no listeners → can skip Java event dispatch.
    bool hasListeners(const std::string& eventType) const;

private:
    FastPropertyStore fastProperties_;

    // Class-level registry of "fast properties" (shared across instances).
    static std::unordered_set<std::string> fastPropertyRegistry_;
};
```

#### `Proxy.cpp` — Modified `onPropertyChangedForProxy`

```cpp
// This is the core change. The existing onPropertyChangedForProxy becomes:
static void onPropertyChangedForProxy(Isolate* isolate, Local<String> property,
                                       Local<Value> value, Local<Object> proxyObject)
{
    Proxy* proxy = NativeObject::Unwrap<Proxy>(proxyObject);
    std::string propName = *v8::String::Utf8Value(isolate, property);

    // === FAST PATH 1: Registered hot property, no listeners ===
    if (proxy->isFastProperty(propName) && !proxy->hasListeners(propName)) {
        proxy->fastProperties().set(isolate, propName, value);
        setPropertyOnProxy(isolate, property, value, proxyObject);
        return;  // 0 JNI operations
    }

    // === FAST PATH 2: Registered hot property, WITH listeners ===
    // Store in C++ + queue dirty + still fire event via JNI
    if (proxy->isFastProperty(propName) && proxy->hasListeners(propName)) {
        proxy->fastProperties().set(isolate, propName, value);
        proxy->fastProperties().markDirty(propName);
        setPropertyOnProxy(isolate, property, value, proxyObject);
        // Still need to notify Java for event dispatch
        // (fall through to existing JNI path but with optimized type conversion)
    }

    // === EXISTING PATH: Full JNI route (unchanged) ===
    JNIEnv* env = JNIScope::getEnv();
    if (!env) {
        LOG_JNIENV_GET_ERROR(TAG);
        return;
    }

    // ... rest of existing JNI code unchanged ...
    // ... jsStringToJavaString, jsValueToJavaObject, CallVoidMethod, cleanup ...
}
```

#### `Proxy.cpp` — `flushDirtyProperties`

```cpp
bool Proxy::flushDirtyProperties(JNIEnv* env)
{
    if (fastProperties_.dirtyProperties().empty()) {
        return false;
    }

    // Pack all dirty changes into a single batch call
    // Use the existing krollProxyOnPropertiesChangedMethod which takes
    // a String[][] of [name, oldValue, newValue] triples.
    // (Same signature as proxyOnPropertiesChanged in the existing code.)

    Isolate* isolate = Isolate::GetCurrent();
    auto& dirty = fastProperties_.dirtyProperties();
    jobjectArray jBatch = env->NewObjectArray(dirty.size(), JNIUtil::objectArrayClass, NULL);

    for (uint32_t i = 0; i < dirty.size(); ++i) {
        const std::string& name = dirty[i];
        v8::HandleScope scope(isolate);
        Local<Value> val = fastProperties_.get(isolate, name);

        jobjectArray jTriple = env->NewObjectArray(3, JNIUtil::objectClass, NULL);
        jstring jName = TypeConverter::jsStringToJavaString(isolate, env,
            v8::String::NewFromUtf8(isolate, name.c_str()).ToLocalChecked());
        env->SetObjectArrayElement(jTriple, 0, jName);
        env->DeleteLocalRef(jName);

        bool isNew;
        jobject jVal = TypeConverter::jsValueToJavaObject(isolate, env, val, &isNew);
        env->SetObjectArrayElement(jTriple, 2, jVal);
        if (isNew) env->DeleteLocalRef(jVal);

        env->SetObjectArrayElement(jBatch, i, jTriple);
        env->DeleteLocalRef(jTriple);
    }

    jobject javaProxy = getJavaObject();
    env->CallVoidMethod(javaProxy, JNIUtil::krollProxyOnPropertiesChangedMethod, jBatch);
    unreferenceJavaObject(javaProxy);
    env->DeleteLocalRef(jBatch);

    if (env->ExceptionCheck()) {
        // handle as existing code does
        env->ExceptionClear();
    }

    fastProperties_.clearDirty();
    return true;
}
```

### Registration System

Properties register as "fast" during proxy type initialization. In the generated
C++ binding code or in a per-proxy-type init function:

```cpp
// Example: registering fast properties for Label
// Called once when the Label proxy template is set up.
void LabelProxy::initFastProperties() {
    Proxy::registerFastProperty("text");
    Proxy::registerFastProperty("color");
    Proxy::registerFastProperty("fontSize");
    Proxy::registerFastProperty("fontWeight");
    Proxy::registerFastProperty("textAlign");
    Proxy::registerFastProperty("shadowOffset");
    Proxy::registerFastProperty("shadowColor");
    Proxy::registerFastProperty("shadowRadius");
    Proxy::registerFastProperty("ellipsize");
    Proxy::registerFastProperty("wordWrap");
    Proxy::registerFastProperty("minimumFontSize");
    Proxy::registerFastProperty("autoLink");
}
```

These are simple value properties that rarely have JS event listeners attached
to them individually. Registering them as "fast" means:

- **Set**: C++ hash map insert — ~50ns, zero JNI.
- **Get**: C++ hash map lookup — ~50ns, zero JNI.
- **Flush**: Batched into a single JNI call with other dirty properties.

Properties not registered (e.g., `creationDict`, custom dynprops) use the
existing full JNI path — no behavior change.

### Flush Trigger

Dirty properties accumulate and flush at well-defined points:

| Trigger | Mechanism | Latency |
|---|---|---|
| **V8 idle callback** | V8's `IdleTask` runs periodically | ~50ms |
| **requestAnimationFrame** | Existing Titanium rAF loop | ~16ms (60fps) |
| **View attachment** | When proxy is attached to a parent view | Immediate |
| **Event firing** | Before `fireEvent()`, flush pending props | Immediate |
| **Explicit `getProperty`** | Java-side read of a pending-dirty property triggers flush | Immediate |

The flush is non-blocking: the C++ store is always the authoritative source,
so reads from JS never wait. The Java side receives batched updates
eventually-consistently.

### Integration Points

1. **`V8Runtime.cpp`**: After `nativeIdle` callback, call `flushDirtyProperties()`
   on all proxies with pending changes.
2. **`TiViewProxy.java`**: After `setParent()` or before view creation, trigger
   flush via new `nativeFlushPendingProperties()` JNI call.
3. **`KrollProxy.java`**: New method `onPropertiesChanged(String[][])` already exists
   (used by the `proxyOnPropertiesChanged` C++ handler). No Java changes needed
   for the batch path.

---

## Files to Create / Modify

### New Files

| File | Contents |
|---|---|
| `android/runtime/v8/src/native/FastPropertyStore.h` | `FastPropertyStore` class declaration |
| `android/runtime/v8/src/native/FastPropertyStore.cpp` | `FastPropertyStore` implementation |

### Modified Files (with change scope)

| File | Changes |
|---|---|
| `android/runtime/v8/src/native/Proxy.h` | Add `fastProperties_` member, `registerFastProperty`, `isFastProperty`, `flushDirtyProperties`, `hasListeners` |
| `android/runtime/v8/src/native/Proxy.cpp` | Modify `onPropertyChangedForProxy()` for fast path; implement `flushDirtyProperties()`; add `hasListeners()` tracking |
| `android/runtime/v8/src/native/V8Runtime.cpp` | Hook idle callback to `flushDirtyProperties` |
| `android/runtime/v8/src/native/JNIUtil.h` | Optionally add view-setter method IDs for Phase 3 direct channel |
| `android/runtime/v8/src/native/JNIUtil.cpp` | Cache the new method IDs |

---

## Implementation Phases

### Phase 1: Core C++ Fast Property Store (estimated: 1–2 weeks)

**Goal:** Properties registered as "fast" can be set and read entirely in C++,
with zero JNI. Dirty properties accumulate and flush on idle callback.

1. Create `FastPropertyStore.h` / `.cpp` with `set`, `get`, `has`, `remove`, `clear`.
2. Add `fastProperties_` member and static `fastPropertyRegistry_` to `Proxy`.
3. Add `registerFastProperty()` / `isFastProperty()` static methods to `Proxy`.
4. Modify `onPropertyChangedForProxy()` in `Proxy.cpp`:
   - Fast property + no listeners → C++ store only, return immediately.
   - Fast property + listeners → C++ store + mark dirty + fall through to JNI.
5. Implement `flushDirtyProperties()` in `Proxy`.
6. Hook flush into `V8Runtime` idle callback.
7. Register a test property (e.g., `text` on Label) as fast in the generated binding code.
8. **Verification:** Use V8 `--trace` or custom logging to confirm JNI is skipped
   for fast property sets but still invoked for non-fast ones.

### Phase 2: Event-Aware Optimization (estimated: 1 week)

**Goal:** Track per-property listener presence so we skip Java event dispatch
for properties that have no listeners.

1. Add `hasListeners()` method to `Proxy` that checks the event listener count
   for the property's change event type.
2. Modify `onPropertyChangedForProxy` to use this check:
   - No listeners → C++ store, skip Java event dispatch.
   - Has listeners → C++ store + fire event via JNI.
3. Caching: store listener presence in a `std::unordered_map<std::string, bool>`
   that gets invalidated when `_hasListenersForEventType` is called.

### Phase 3: View-Registered "Hot Property" Registry (estimated: 1 week)

**Goal:** API for proxy subclasses to declare their fast-hot properties declaratively.

1. Define an annotation or static initializer convention in the generated C++ bindings.
2. Generate `initFastProperties()` calls from the existing binding generator.
3. Add a `TITANIUM_FAST_PROPERTIES(prop1, prop2, ...)` macro to make registration
   concise in C++ proxy headers.

### Phase 4: (Optional) Direct View Channel — Android (estimated: 2 weeks)

**Goal:** Bypass KrollProxy → TiViewProxy → TiUIView chain entirely for
properties that map directly to Android View setters.

1. Cache `JNIUtil` method IDs for `TextView.setText()`, `TextView.setTextColor()`,
   `TextView.setTextSize()`, etc.
2. In `Proxy::flushDirtyProperties()`, if the Java proxy class has a registered
   "direct setter" for the property, call the View setter JNI method directly.
3. Fall back to the normal `onPropertiesChanged` path for non-direct properties.

### Phase 5: iOS Port (estimated: 1–2 weeks)

**Goal:** Same C++ property store, with an ObjC flush backend for iOS.

1. Compile same `FastPropertyStore.cpp` in the iOS project (Objective-C++ `.mm`).
2. JSC equivalent of the V8 setter interceptor attaches a C++ `Proxy` to the
   JS object via `JSObjectSetPrivate`.
3. Store properties in C++; flush uses `objc_msgSend` directly to the UIView
   instead of going through `KrollObject.setValue:forKey:`.
4. **Verification:** Same property-set pattern shows no ObjC KVC dispatch
   for fast properties.

---

## iOS Port

### Current iOS Call Path

```
[JS]     label.text = 'hello'
  ↓
[JSC]    KrollSetProperty (C function, KrollObject.m:260)
  ↓
[ObjC]   KrollObject.setValue:forKey:  (via KVC)
  ↓
[ObjC]   TiProxy.setValue:forKey:
  ↓
[ObjC]   TiUIView (modelDelegate) → [(UILabel*)view setText:@"hello"]
```

The iOS path is already faster than Android (ObjC runtime dispatch vs. JNI),
but still goes through KVC string parsing, KrollObject intermediate dispatch,
and TiProxy generic `setValue:forKey:`.

### iOS with JSI Binding

1. The same `FastPropertyStore` C++ class is compiled into the iOS framework
   (via Objective-C++ `.mm` files).
2. The JSC setter callback stores the property value in the C++ store
   instead of calling `setValue:forKey:` on the ObjC KrollObject.
3. Flush calls the UIKit setter directly:
   ```objc
   [(UILabel*)view setText:value];  // direct message, no KVC
   ```
4. If no listeners and no view update needed, flush to ObjC is skipped entirely.

### Platform Comparison

| Aspect | Android | iOS |
|---|---|---|
| Fast property store | Same `FastPropertyStore` C++ class | Same `FastPropertyStore` C++ class |
| Flush target | `KrollProxy.onPropertiesChanged()` (Java) | `[(UIView)view set<Prop>]` (ObjC direct) |
| View reference | JNI global ref via ReferenceTable | `__unsafe_unretained UIView*` |
| Listener check | `KrollObject.hasListeners()` via JNI | `TiProxy._listeners` count via ObjC |
| JS setter interceptor | V8 `NamedPropertySetter` | JSC `JSClassSetProperty` callback |

---

## Performance Projections

### Measured JNI Overhead (approx)

| Operation | Approx time |
|---|---|
| `TypeConverter::jsStringToJavaString` | ~200ns (UTF-16 alloc) |
| `TypeConverter::jsValueToJavaObject` | ~100–300ns (type dispatch) |
| `proxy->getJavaObject()` | ~50ns (ReferenceTable lookup) |
| `CallVoidMethod` (JNI boundary) | ~50–150ns |
| `unreferenceJavaObject` + `DeleteLocalRef` × 2 | ~50ns |
| **Total per property set (current)** | **~450–750ns** |

### Projected Savings

| Scenario | Current | JSI Binding | Savings |
|---|---|---|---|
| `label.text = 'hello'` (no listeners) | ~600ns | ~50ns (C++ hash put) | **92%** |
| `label.text` (get back) | ~400ns | ~50ns (C++ hash get) | **87%** |
| Creation dict (10 props, no listeners) | ~6μs | ~500ns (C++ puts) + batch flush | **90%+** |
| `label.text = 'hello'` (with listeners) | ~600ns | ~500ns (C++ put + JNI event) | **~17%** |
| Scroll view cell update (8 props) | ~4.8μs | ~400ns (C++ puts) + 1 flush call | **~70%** |

### Impact on Real-World Scenarios

| Scenario | Improvement |
|---|---|
| **Table/List view scrolling** (cell update in `drawRect` / `willDisplayCell`) | ~5–10x faster property application per cell |
| **Animation** (continuous property sets) | Eliminate JNI per frame for animated properties |
| **Initial window/scrollView creation** (creation dict with 20+ properties) | ~10x faster initial property population |
| **Normal app interactions** (single property sets, user-initiated) | ~2–3x faster for common operations |

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| **C++ persistent values per property increase memory** | ~Persistent<V8 Value> = ~40 bytes + V8 heap overhead per stored property | Cap fast property count per proxy at ~32; evict least-recently-set; fall through to JNI path beyond cap |
| **Stale Java state if C++ and Java get out of sync** | Java reads (e.g., reflection, native module access) return stale values | Flush before any Java-side read; make C++ authoritative on flush; all reads route through C++ |
| **Thread safety — V8 thread sets, UI thread flushes** | Concurrent modification of `std::unordered_map` | Guard with `v8::Locker` (already in place); flush always runs on V8 thread; UI thread reads are deferred |
| **Backward compatibility break for properties with Java side effects** | Property has unexpected Java behavior (startActivity, database write) | Opt-in per property; default is existing JNI path; only well-known simple-value properties are registered |
| **Increased complexity in debugging** | Harder to trace property flow when it can skip Java | Add `TRACE`-level logging for fast path (off by default); include property origin in debug builds |
| **Kroll `_properties` map is now duplicated** | V8 internal map + C++ map hold same data | Could eliminate the V8 map for fast properties entirely (store only in C++), but keep both for backward compat in Phase 1; optimize in later phase |
| **iOS-at-Android parity gap** | iOS View channel calls UIKit directly, Android calls Java proxy still | Acceptable — both are faster than current; direct Android View channel is Phase 4 |

---

## Appendix: Glossary

| Term | Definition |
|---|---|
| **JNI** | Java Native Interface — mechanism for C++ to call Java methods on Android |
| **JSI** | JavaScript Interface — React Native's C++ layer for synchronous JS-to-native calls (inspiration for this design) |
| **Kroll** | Titanium's internal binding framework that connects JavaScript to native code |
| **KrollProxy** | Java base class for all Titanium proxy objects that are exposed to JavaScript |
| **TiViewProxy** | Java proxy subclass that manages a native Android View |
| **TiUIView** | Java class that wraps an Android View and receives property changes from its proxy |
| **ReferenceTable** | Java/C++ table that maps integer IDs to JNI object references (strong/weak) |
| **V8 Persistent** | V8 API for a reference to a JS value that persists across garbage collections |
| **Fast Property** | A property registered to skip JNI and use the C++ store (opt-in) |
| **Dirty Property** | A fast property whose value changed in C++ but hasn't been flushed to Java yet |
