/**
 * Titanium SDK
 * Copyright TiDev, Inc. 04/07/2022-Present
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */

#include "FastPropertyStore.h"

namespace titanium {

FastPropertyStore::~FastPropertyStore()
{
	clear();
}

void FastPropertyStore::set(const std::string& name, v8::Local<v8::Value> value)
{
	v8::Isolate* isolate = v8::Isolate::GetCurrent();
	auto it = map_.find(name);
	if (it != map_.end()) {
		it->second.Reset(isolate, value);
	} else {
		v8::Persistent<v8::Value, v8::CopyablePersistentTraits<v8::Value>> persistent(isolate, value);
		map_.emplace(name, std::move(persistent));
	}
}

v8::Local<v8::Value> FastPropertyStore::get(v8::Isolate* isolate, const std::string& name) const
{
	auto it = map_.find(name);
	if (it != map_.end() && !it->second.IsEmpty()) {
		return it->second.Get(isolate);
	}
	return v8::Local<v8::Value>();
}

bool FastPropertyStore::has(const std::string& name) const
{
	auto it = map_.find(name);
	return it != map_.end() && !it->second.IsEmpty();
}

void FastPropertyStore::remove(const std::string& name)
{
	auto it = map_.find(name);
	if (it != map_.end()) {
		it->second.Reset();
		map_.erase(it);
	}
}

void FastPropertyStore::clear()
{
	for (auto& entry : map_) {
		entry.second.Reset();
	}
	map_.clear();
	dirtyProperties_.clear();
}

void FastPropertyStore::markDirty(const std::string& name)
{
	dirtyProperties_.push_back(name);
}

} // namespace titanium
