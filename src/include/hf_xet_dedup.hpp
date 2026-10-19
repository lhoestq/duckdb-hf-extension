//===----------------------------------------------------------------------===//
// The client's half of cross-repository dedup (docs/XET.md, M9 (C) + (D)).
//
// A CAS dedup query answers with a metadata shard, and a shard says two things: which xorbs exist, and
// which chunks each one holds. This holds those two *by value* - the answer arrives as bytes owned by an
// HTTP response, a run is matched several queries after the shard that makes it possible arrived, and a
// view borrowed out of a string that has since been freed reads as a chunk hash nobody ever uploaded.
//
// What it can then answer is the one question the upload path asks: "starting at this chunk of the file I
// am writing, how much of it is already stored, and where?" A matched chunk is never uploaded; the new
// shard points into an xorb somebody else already paid for. Which is the whole economic argument for Xet,
// and the half that a round-trip test of your own upload cannot see: it is about content *someone else*
// stored.
//===----------------------------------------------------------------------===//

#pragma once
#include "hf_cas.hpp"
#include "hf_common.hpp"
#include "hf_xet_hash.hpp"
#include "hf_xet_shard.hpp"

#include <unordered_map>

namespace duckdb {

//! How far apart two dedup queries may be, counted in chunks. This is the reference's
//! `min_spacing_between_global_dedup_queries`, default 256, which at ~64 KiB chunks means "no more than
//! one query per ~16 MB". It is not a rate limit so much as a statement about content: eligible chunks
//! are one hash in 1024, so querying denser than that mostly re-asks a question already asked.
constexpr idx_t HF_XET_DEDUP_QUERY_SPACING_CHUNKS = 256;

//! Where a stored chunk lives, in this table's own coordinates: which xorb of the table's, and which
//! position inside that xorb. u32s because those are the numbers a shard segment carries, not because a
//! limit was measured - a xorb holds at most HF_XET_MAX_XORB_CHUNKS chunks anyway.
struct HFXetDedupLocation {
	uint32_t xorb;
	uint32_t chunk;
};

//! A run of a file's chunks that the CAS can already serve. `count` chunks starting at `file_chunk`, all
//! of them contiguous inside one xorb. Turning this into a shard segment means copying the *referenced
//! xorb's* flags and summing the run's uncompressed bytes - which is exactly what the reference's
//! `FileDataSequenceEntry::from_xorb_entries` does, and a segment that disagrees with the xorb it names
//! is a shard the CAS refuses to verify.
struct HFXetDedupRun {
	idx_t file_chunk = 0;
	idx_t count = 0;
	HFXetHash xorb_hash;
	uint32_t xorb_flags = 0;
	//! positions inside `xorb_hash`'s own chunk list, not the file's
	uint32_t chunk_index_start = 0;
	uint32_t chunk_index_end = 0;
	uint64_t unpacked_bytes = 0;
};

//! The chunks known to be stored, from the shards the CAS handed back.
class HFXetDedupTable {
public:
	//! Take in everything the CAS said about one chunk. The shard describes several xorbs and however many
	//! chunks they hold - it is a whole shard, not an answer about one chunk - and all of it becomes
	//! matchable, because a run found at an eligible chunk usually carries on past it. Returns how many
	//! chunk entries were not already known: two queries answered by the same shard add nothing twice.
	idx_t Register(const HFXetShardView &shard);

	//! The longest run of `hashes` starting at `start` that this table holds contiguously, or 0 when it
	//! does not know that chunk at all (the common case, and the cheap one). The rule is the reference's
	//! (`ShardInMemory::chunk_hash_dedup_query`): the first hash decides a xorb and a position in it, then
	//! file position and xorb position advance together until either the hashes stop agreeing or the
	//! xorb's chunk list runs out. A run therefore never straddles two xorbs.
	//!
	//! `hashes` are the file's own, unkeyed chunk hashes: each collection is asked in its own key, so a
	//! shard that protects its entries gets a keyed question and an unkeyed one gets a plain question.
	idx_t Match(const vector<HFXetHash> &hashes, idx_t start, HFXetDedupRun &run) const;

	//! Whether this chunk hash is known at all - which is not the same as knowing a run *starts* here,
	//! since a chunk in the middle of a run is known too. Asked of every collection, in its own key.
	bool Knows(const HFXetHash &hash) const;

	idx_t XorbCount() const {
		idx_t total = 0;
		for (auto &collection : collections) {
			total += collection.xorbs.size();
		}
		return total;
	}
	idx_t ChunkCount() const {
		idx_t total = 0;
		for (auto &collection : collections) {
			total += collection.lookup.size();
		}
		return total;
	}
	//! How many distinct shard keys the registered shards have spoken with. Usually one; more than one
	//! means the CAS answered with shards from more than one collection, each to be asked in its own key.
	idx_t KeyCount() const {
		return collections.size();
	}

private:
	//! A xorb's chunk table, copied out of whatever shard described it
	struct Xorb {
		HFXetHash hash;
		uint32_t flags = 0;
		vector<HFXetShardChunkView> chunks;
	};

	//! Keying the lookup by word 0 is the reference's own choice of digest prefix - `truncate_hash`, the
	//! first hash word, is what shards order and index their entries by - so it is the word whose spread
	//! is already load-bearing here.
	struct Key {
		size_t operator()(const HFXetHash &hash) const {
			return static_cast<size_t>(hash.Word(0));
		}
	};

	//! The chunks described by shards that share one key. A stored shard protects its chunk hashes with a
	//! key of its own (`MDBShardFileFooter.chunk_hash_hmac_key`), so "is this chunk stored?" is a question
	//! that has to be asked once per key - which is the reference's `KeyedShardCollection`, and the reason
	//! this is a list of tables rather than one table: a single unkeyed map would mix hashes that are keyed
	//! with hashes that are not, and answer nothing about either.
	struct Collection {
		explicit Collection(const HFXetHash &key_p) : key(key_p) {
		}
		//! Zero means these shards store their chunk hashes as they were given
		HFXetHash key;
		vector<Xorb> xorbs;
		std::unordered_map<HFXetHash, uint32_t, Key> xorbs_by_hash;
		//! chunk hashes as the shards spelled them - already keyed, when they are keyed
		std::unordered_map<HFXetHash, HFXetDedupLocation, Key> lookup;
	};

	vector<Collection> collections;
};

//! What asking the CAS cost, and what it was worth.
struct HFXetDedupQueryReport {
	idx_t queries = 0;
	//! queries answered with a shard, out of `queries`
	idx_t hits = 0;
	//! chunk entries those shards made matchable
	idx_t registered_chunks = 0;
	//! shards that came back and could not be parsed. They are not fatal - an upload that dedups nothing is
	//! still a correct upload - but they are not nothing either, and a parser that quietly stopped working
	//! would otherwise show up as a file that never finds a match.
	idx_t parse_failures = 0;
};

//! Which positions of a file's chunk list are worth a query: its first chunk, and every chunk the hash
//! rule makes eligible at least HF_XET_DEDUP_QUERY_SPACING_CHUNKS after the last position asked about.
//!
//! A chunk the table already holds is passed over, which is the reference's own tier order at work (it
//! consults the session and resume caches before the global index) - and passing over it does not spend
//! the spacing, because in the reference the counter moves when a query goes out, not when a chunk is
//! looked at.
vector<idx_t> HFXetDedupCandidates(const vector<HFXetHash> &hashes, const HFXetDedupTable &table);

//! Ask the CAS about the chunks of a file it could possibly have indexed, and register every shard it
//! hands back. `hashes` is the file's chunk hashes in file order.
//!
//! Two kinds of position get asked about, and no others: a file's first chunk, which is the one chunk
//! every uploader's shard marks with a flag and therefore the one that is always in the index (this is
//! how an identical file is found at all), and chunks the hash rule makes eligible
//! (`HFXetGlobalDedupEligible`). Asking about anything else is a round trip for a 404 - measured over the
//! live CAS at 13 misses out of 13 such queries.
//!
//! Every hit is asked about *before* anything is uploaded, which is what lets a match be acted on rather
//! than discovered afterwards: the reference runs the same two passes, and issues its queries from the
//! first one.
HFXetDedupQueryReport HFXetDedupQuery(HFXetCasClient &cas, const vector<HFXetHash> &hashes, HFXetDedupTable &table);

//===----------------------------------------------------------------------===//
// Self-test
//===----------------------------------------------------------------------===//

//! Matching is the part of this that can be checked without a network: the shards are ours to build, and
//! a matcher that stops one chunk early costs an upload while looking exactly like one that found
//! nothing to dedup. So the checks are about boundaries - a run ending at a xorb's last chunk, a run
//! broken by one differing hash in the middle, a run that would like to continue into a second xorb and
//! may not - plus what a run carries out of a shard (the referenced xorb's flags, the summed bytes).
idx_t HFXetDedupSelfTest(vector<std::tuple<string, bool, string, string>> &results);

} // namespace duckdb
