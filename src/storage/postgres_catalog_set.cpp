#include "storage/postgres_catalog_set.hpp"
#include "storage/postgres_transaction.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "storage/postgres_schema_entry.hpp"

#include <absl/cleanup/cleanup.h>

#include <bit>
#include <thread>

namespace duckdb {

static constexpr uint64_t kUnloaded = 0;
static constexpr uint64_t kLoaded = ~kUnloaded;

PostgresCatalogSet::PostgresCatalogSet(Catalog &catalog, bool is_loaded_p)
    : catalog(catalog), loader_state(is_loaded_p ? kLoaded : kUnloaded) {
}

optional_ptr<CatalogEntry> PostgresCatalogSet::GetEntry(ClientContext &context, PostgresTransaction &transaction,
                                                        const string &name) {
	TryLoadEntries(context, transaction);
	{
		lock_guard<mutex> l(entry_lock);
		auto entry = entries.find(name);
		if (entry != entries.end()) {
			// entry found
			return transaction.ReferenceEntry(entry->second);
		}
		// check the case insensitive map if there are any entries
		auto name_entry = entry_map.find(name);
		if (name_entry != entry_map.end()) {
			// try again with the entry we found in the case insensitive map
			auto entry = entries.find(name_entry->second);
			if (entry != entries.end()) {
				// still not found
				return transaction.ReferenceEntry(entry->second);
			}
		}
	}
	// entry not found
	if (SupportReload()) {
		if (loader_state.load(std::memory_order_relaxed) ==
		    std::bit_cast<uint64_t>(std::this_thread::get_id())) {
			return nullptr;
		}
		lock_guard<mutex> lock(load_lock);
		// try loading entries again - maybe there has been a change remotely
		auto entry = ReloadEntry(transaction, name);
		if (entry) {
			return entry;
		}
	}
	return nullptr;
}

void PostgresCatalogSet::TryLoadEntries(ClientContext &context, PostgresTransaction &transaction) {
	const auto state = loader_state.load(std::memory_order_relaxed);
	if (state == kLoaded) {
		return;
	}
	const auto self = std::bit_cast<uint64_t>(std::this_thread::get_id());
	if (state == self) {
		return;
	}
	lock_guard<mutex> lock(load_lock);
	if (loader_state.load(std::memory_order_relaxed) == kLoaded) {
		return;
	}
	loader_state.store(self, std::memory_order_relaxed);
	absl::Cleanup rollback = [this] noexcept {
		lock_guard<mutex> entry_guard(entry_lock);
		entry_map.clear();
		entries.clear();
		loader_state.store(kUnloaded, std::memory_order_relaxed);
	};
	LoadEntries(context, transaction);
	loader_state.store(kLoaded, std::memory_order_relaxed);
	std::move(rollback).Cancel();
}

optional_ptr<CatalogEntry> PostgresCatalogSet::ReloadEntry(PostgresTransaction &transaction, const string &name) {
	throw InternalException("PostgresCatalogSet does not support ReloadEntry");
}

void PostgresCatalogSet::DropEntry(PostgresTransaction &transaction, DropInfo &info) {
	string drop_query = "DROP ";
	drop_query += CatalogTypeToString(info.type) + " ";
	if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
		drop_query += " IF EXISTS ";
	}
	if (!info.schema.empty()) {
		drop_query += KeywordHelper::WriteQuoted(info.schema, '"') + ".";
	}
	drop_query += KeywordHelper::WriteQuoted(info.name, '"');
	if (info.cascade) {
		drop_query += "CASCADE";
	}
	transaction.Query(drop_query);

	lock_guard<mutex> load_guard(load_lock);
	lock_guard<mutex> entry_guard(entry_lock);
	if (entries.erase(info.name) == 0) {
		return;
	}
	auto name_it = entry_map.find(info.name);
	if (name_it != entry_map.end() && name_it->second == info.name) {
		entry_map.erase(name_it);
	}
}

void PostgresCatalogSet::Scan(ClientContext &context, PostgresTransaction &transaction,
                              const std::function<void(CatalogEntry &)> &callback) {
	TryLoadEntries(context, transaction);
	lock_guard<mutex> l(entry_lock);
	for (auto &entry : entries) {
		callback(*transaction.ReferenceEntry(entry.second));
	}
}

optional_ptr<CatalogEntry> PostgresCatalogSet::CreateEntry(PostgresTransaction &transaction,
                                                           shared_ptr<CatalogEntry> entry) {
	lock_guard<mutex> l(entry_lock);
	if (entry->name.empty()) {
		throw InternalException("PostgresCatalogSet::CreateEntry called with empty name");
	}
	auto name = entry->name;
	entry_map.emplace(name, name);
	auto [it, inserted] = entries.emplace(name, std::move(entry));
	return transaction.ReferenceEntry(it->second);
}

void PostgresCatalogSet::ClearEntries() {
	lock_guard<mutex> load_guard(load_lock);
	lock_guard<mutex> entry_guard(entry_lock);
	entry_map.clear();
	entries.clear();
	loader_state.store(kUnloaded, std::memory_order_relaxed);
}

void PostgresCatalogSet::MarkUnloaded() {
	loader_state.store(kUnloaded, std::memory_order_relaxed);
}

PostgresInSchemaSet::PostgresInSchemaSet(PostgresSchemaEntry &schema, bool is_loaded)
    : PostgresCatalogSet(schema.ParentCatalog(), is_loaded), schema(schema) {
}

optional_ptr<CatalogEntry> PostgresInSchemaSet::CreateEntry(PostgresTransaction &transaction,
                                                            shared_ptr<CatalogEntry> entry) {
	entry->internal = schema.internal;
	return PostgresCatalogSet::CreateEntry(transaction, std::move(entry));
}

} // namespace duckdb
