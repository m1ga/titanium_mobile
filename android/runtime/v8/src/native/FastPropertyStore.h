/**
 * Titanium SDK
 * Copyright TiDev, Inc. 04/07/2022-Present
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */

#ifndef FAST_PROPERTY_STORE_H_
#define FAST_PROPERTY_STORE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <v8.h>
#include <jni.h>

namespace titanium {

// C++-authoritative property store that skips JNI for registered "fast" properties.
// Properties stored here are read back from C++ without crossing the JNI boundary.
// Dirty properties accumulate and are flushed to Java in a single batched JNI call.
class FastPropertyStore {
public:
	FastPropertyStore() = default;
	~FastPropertyStore();

	FastPropertyStore(const FastPropertyStore&) = delete;
	FastPropertyStore& operator=(const FastPropertyStore&) = delete;

	// Store a property value. Takes ownership of a persistent handle.
	void set(const std::string& name, v8::Local<v8::Value> value);

	// Retrieve a property value. Returns empty Local if not found.
	v8::Local<v8::Value> get(v8::Isolate* isolate, const std::string& name) const;

	// Check if a property exists in this store.
	bool has(const std::string& name) const;

	// Remove a property from this store.
	void remove(const std::string& name);

	// Clear all stored properties.
	void clear();

	// Mark a property as dirty (needs flush to Java).
	void markDirty(const std::string& name);

	// Returns the list of dirty property names.
	const std::vector<std::string>& dirtyProperties() const { return dirtyProperties_; }

	// Clear the dirty list.
	void clearDirty() { dirtyProperties_.clear(); }

	// Returns true if any properties are dirty.
	bool hasDirty() const { return !dirtyProperties_.empty(); }

private:
	using PropertyMap = std::unordered_map<
		std::string,
		v8::Persistent<v8::Value, v8::CopyablePersistentTraits<v8::Value>>
	>;

	PropertyMap map_;
	std::vector<std::string> dirtyProperties_;
};

} // namespace titanium

#endif
