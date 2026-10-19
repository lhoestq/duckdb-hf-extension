#include "hf_xet_dedup.hpp"

#include "hf_xet_chunk.hpp"

#include "hf_xet_chunk.hpp"

namespace duckdb {

idx_t HFXetDedupTable::Register(const HFXetShardView &shard) {
	// One table per distinct footer key, which is how the reference arranges the same knowledge: shards are
	// grouped into `KeyedShardCollection`s keyed by `chunk_hash_hmac_key`, and a query is keyed once per
	// collection rather than the table being unkeyed. Unkeying a stored shard on the way in is not an option
	// anyway - the key protects the hashes, and nothing but the shard knows how to undo it.
	Collection *collection = nullptr;
	for (auto &existing : collections) {
		if (existing.key == shard.chunk_hash_hmac_key) {
			collection = &existing;
			break;
		}
	}
	if (!collection) {
		collections.emplace_back(shard.chunk_hash_hmac_key);
		collection = &collections.back();
	}
	idx_t added = 0;
	for (auto &view : shard.xorbs) {
		if (collection->xorbs_by_hash.count(view.hash) > 0) {
			// Two queries answered by the same shard reach this line twice, and a shard that names an xorb
			// another shard already described lands here too. It is not merged: a xorb is addressed by the
			// hash of its chunks, so a second description of the same address can only agree - and keeping
			// both would double the memory while giving the lookup nothing to find.
			continue;
		}
		Xorb owned;
		owned.hash = view.hash;
		owned.flags = view.flags;
		owned.chunks = view.chunks; //! the copy this class exists for: `view` dies with the response bytes
		collection->xorbs.push_back(std::move(owned));
		auto index = uint32_t(collection->xorbs.size() - 1);
		collection->xorbs_by_hash.emplace(view.hash, index);

		for (idx_t i = 0; i < collection->xorbs[index].chunks.size(); i++) {
			// Stored exactly as the shard spelled it. For a keyed shard that spelling is already
			// `keyed_chunk_hash(raw)` - the CAS did the keying when it indexed, and keying it again here
			// would put the table one hash away from everything it is asked about. The key is applied to the
			// *query* side only, which is what `MDBShardInfo::keyed_chunk_hash` is for and what Match does.
			const auto &stored = collection->xorbs[index].chunks[i].hash;
			// Keep the first mention of a chunk hash, which is what the reference does when it merges a
			// shard into a table that already has content (`from_new_shard` filters the hashes it already
			// knows rather than overwriting them). Any of the locations holding a hash describes the same
			// bytes, but *which* one answers changes the shard bytes an upload then produces, so the rule
			// is stated here rather than left to whatever the container did.
			auto result = collection->lookup.emplace(stored, HFXetDedupLocation {index, uint32_t(i)});
			if (result.second) {
				added++;
			}
		}
	}
	return added;
}

idx_t HFXetDedupTable::Match(const vector<HFXetHash> &hashes, idx_t start, HFXetDedupRun &run) const {
	if (start >= hashes.size()) {
		return 0;
	}
	for (auto &collection : collections) {
		// One pass per keyed collection, the reference's order (`ShardFileManager::chunk_hash_dedup_query`
		// walks `shard_collections` and lets the first one that resolves the hash answer). Asking a
		// collection about a chunk is the keyed form; the file's own hashes are never keyed.
		auto entry = collection.lookup.find(HFXetKeyedChunkHash(hashes[start], collection.key));
		if (entry == collection.lookup.end()) {
			continue;
		}
		auto &xorb = collection.xorbs[entry->second.xorb];
		idx_t position = entry->second.chunk;

		run.xorb_hash = xorb.hash;
		run.xorb_flags = xorb.flags;
		run.chunk_index_start = uint32_t(position);
		run.file_chunk = start;

		idx_t count = 0;
		uint64_t bytes = 0;
		// Both bounds are load-bearing. The first stops a run from running off the end of the xorb that holds
		// it - a shard segment addresses one xorb, so a match that would continue into the next one is not one
		// segment's business. The second stops it at the end of the file being written.
		while (position < xorb.chunks.size() && start + count < hashes.size() &&
		       xorb.chunks[position].hash == HFXetKeyedChunkHash(hashes[start + count], collection.key)) {
			bytes += xorb.chunks[position].unpacked_bytes;
			position++;
			count++;
		}
		if (count == 0) {
			// the lookup said yes and the entries said no: the reference treats that as "this collection did
			// not answer" and keeps going rather than returning an empty run
			continue;
		}
		run.count = count;
		run.chunk_index_end = uint32_t(position);
		run.unpacked_bytes = bytes;
		return count;
	}
	return 0;
}

bool HFXetDedupTable::Knows(const HFXetHash &hash) const {
	for (auto &collection : collections) {
		if (collection.lookup.count(HFXetKeyedChunkHash(hash, collection.key)) > 0) {
			return true;
		}
	}
	return false;
}

vector<idx_t> HFXetDedupCandidates(const vector<HFXetHash> &hashes, const HFXetDedupTable &table) {
	vector<idx_t> candidates;
	idx_t next = 0;
	for (idx_t i = 0; i < hashes.size(); i++) {
		if (i < next) {
			continue;
		}
		// A file's first chunk is asked about whatever its hash says, because it is the chunk an uploader's
		// shard marks with a flag - which makes it the only chunk that is in the index *for sure*, and the
		// reason an identical file costs one query to find.
		if (i != 0 && !HFXetGlobalDedupEligible(hashes[i])) {
			continue;
		}
		if (table.Knows(hashes[i])) {
			// Already hold a shard that names this chunk (an earlier query answered it, usually with more
			// than the chunk asked about). Not asking again does not spend the spacing either: like the
			// reference, the counter moves when a query goes out, not when a chunk is looked at.
			continue;
		}
		candidates.push_back(i);
		next = i + HF_XET_DEDUP_QUERY_SPACING_CHUNKS;
	}
	return candidates;
}

HFXetDedupQueryReport HFXetDedupQuery(HFXetCasClient &cas, const vector<HFXetHash> &hashes, HFXetDedupTable &table) {
	HFXetDedupQueryReport report;
	for (auto index : HFXetDedupCandidates(hashes, table)) {
		string shard_bytes;
		report.queries++;
		if (!cas.QueryChunk(hashes[index], shard_bytes)) {
			continue;
		}
		report.hits++;
		try {
			auto shard = HFXetShardView::Parse(shard_bytes);
			report.registered_chunks += table.Register(shard);
		} catch (std::exception &ex) {
			// Dedup is the part of an upload that can fail and leave the upload correct, so a shard this
			// parser cannot read does not stop the file: the content it describes is simply not pointed at,
			// and the bytes go up instead. Counted rather than swallowed, because "dedup never hits" and
			// "dedup cannot parse what it gets" look identical from the outside and are not.
			report.parse_failures++;
		}
	}
	return report;
}

//===----------------------------------------------------------------------===//
// Self-test
//===----------------------------------------------------------------------===//

namespace {

//! A chunk hash from bytes, so the tables below hold hashes of the kind that occur here rather than
//! hand-written hex that could accidentally be a digest of nothing.
HFXetHash DedupTestHash(const string &content) {
	return HFXetChunkHash(const_data_ptr_cast(content.data()), content.size());
}

HFXetShardChunkView DedupTestChunk(const string &content, uint32_t &byte_range_start, uint32_t unpacked,
                                   uint32_t flags = 0) {
	HFXetShardChunkView chunk;
	chunk.hash = DedupTestHash(content);
	chunk.byte_range_start = byte_range_start;
	chunk.unpacked_bytes = unpacked;
	chunk.flags = flags;
	byte_range_start += unpacked;
	return chunk;
}

string DedupCandidatesText(const vector<idx_t> &candidates) {
	string out;
	for (auto index : candidates) {
		if (!out.empty()) {
			out += ",";
		}
		out += to_string(index);
	}
	return out.empty() ? "none" : out;
}

} // namespace

idx_t HFXetDedupSelfTest(vector<std::tuple<string, bool, string, string>> &results) {
	idx_t failures = 0;
	auto check = [&](const string &name, const string &expected, const string &got) {
		results.emplace_back(name, expected == got, expected, got);
		if (expected != got) {
			failures++;
		}
	};

	// Two xorbs of four chunks each, described by a shard view built the way a parsed one would look:
	// xorb-local chunk order, byte ranges accumulating inside each xorb, and one flag value per xorb so a
	// run's flags are traceable to where they came from.
	HFXetShardView shard;
	uint32_t offset = 0;
	for (idx_t x = 0; x < 2; x++) {
		HFXetShardXorbView xorb;
		xorb.hash = DedupTestHash(string("xorb ") + to_string(x));
		xorb.flags = x == 0 ? 0x00000007u : 0u;
		offset = 0;
		for (idx_t c = 0; c < 4; c++) {
			xorb.chunks.push_back(DedupTestChunk(string("chunk ") + to_string(x) + "-" + to_string(c), offset,
			                                      uint32_t(1000 + c)));
		}
		shard.xorbs.push_back(std::move(xorb));
	}
	// the file entry is what a CAS response carries too, and registering ignores it: a run is about xorbs
	HFXetShardFileView file;
	file.file_hash = DedupTestHash("a file");
	shard.files.push_back(file);

	HFXetDedupTable table;
	check("dedup table registers the first shard", "8", to_string(table.Register(shard)));
	check("dedup table holds both xorbs", "2", to_string(table.XorbCount()));
	check("dedup table re-registering a shard adds nothing", "0", to_string(table.Register(shard)));

	// The file whose whole content is the two xorbs' chunks in order, so every run's length says where a
	// xorb ends.
	vector<HFXetHash> whole;
	for (auto &xorb : shard.xorbs) {
		for (auto &chunk : xorb.chunks) {
			whole.push_back(chunk.hash);
		}
	}

	HFXetDedupRun run;
	check("dedup match takes the whole first xorb", "4", to_string(table.Match(whole, 0, run)));
	check("dedup run names the xorb it found", shard.xorbs[0].hash.Hex(), run.xorb_hash.Hex());
	check("dedup run carries xorb-local indices", "0-4",
	      to_string(run.chunk_index_start) + "-" + to_string(run.chunk_index_end));
	check("dedup run sums the chunks' uncompressed bytes", "4006", to_string(run.unpacked_bytes));
	check("dedup run carries the referenced xorb's flags", "7", to_string(run.xorb_flags));
	// The next position starts a run of its own, in the other xorb: two segments, not one that straddles.
	check("dedup match stops at the end of a xorb", "4", to_string(table.Match(whole, 4, run)));
	check("dedup second run names the second xorb", shard.xorbs[1].hash.Hex(), run.xorb_hash.Hex());
	check("dedup second run carries its flags", "0", to_string(run.xorb_flags));

	// A run that starts in the middle of a xorb: the lookup is by hash, so the entry point is the xorb's
	// third chunk and the segment has to say so.
	vector<HFXetHash> middle(whole.begin() + 2, whole.begin() + 4);
	check("dedup match resumes mid-xorb", "2", to_string(table.Match(middle, 0, run)));
	check("dedup mid-xorb run starts where the lookup landed", "2", to_string(run.chunk_index_start));
	check("dedup mid-xorb run index range", "2-4",
	      to_string(run.chunk_index_start) + "-" + to_string(run.chunk_index_end));

	// One differing hash in the middle stops it there rather than skipping past it.
	vector<HFXetHash> broken = whole;
	broken[2] = DedupTestHash("content nobody else has");
	check("dedup match breaks on a differing hash", "2", to_string(table.Match(broken, 0, run)));

	// Unknown first hash: nothing to report, and the common case by a wide margin.
	vector<HFXetHash> unknown {DedupTestHash("nothing here"), whole[1]};
	check("dedup match on an unknown chunk finds nothing", "0", to_string(table.Match(unknown, 0, run)));
	check("dedup knows a chunk in the middle of a run", "true", table.Knows(whole[3]) ? "true" : "false");
	check("dedup does not know content it was never told about", "false",
	      table.Knows(DedupTestHash("never registered")) ? "true" : "false");

	// A hash held by two xorbs keeps the first location. What is being defended is not correctness of the
	// bytes - both locations describe the same content - but the stability of the shard an upload then
	// writes, which is the thing a reference vector can compare.
	HFXetShardView second_shard;
	HFXetShardXorbView overlapping;
	overlapping.hash = DedupTestHash("xorb 2 again");
	overlapping.flags = 0;
	offset = 0;
	overlapping.chunks.push_back(DedupTestChunk("chunk 0-2", offset, 1002)); // already held by xorb 0
	overlapping.chunks.push_back(DedupTestChunk("chunk fresh", offset, 500));
	second_shard.xorbs.push_back(std::move(overlapping));
	check("dedup registers only the unseen chunk of an overlapping shard", "1",
	      to_string(table.Register(second_shard)));
	auto first_of_overlap = DedupTestHash("chunk 0-2");
	vector<HFXetHash> overlap_query {first_of_overlap};
	table.Match(overlap_query, 0, run);
	check("dedup keeps the first location for a hash two xorbs hold", shard.xorbs[0].hash.Hex(),
	      run.xorb_hash.Hex());

	//===----------------------------------------------------------------------===//
	// A shard in the shape a dedup query answers with
	//===----------------------------------------------------------------------===//

	// Everything above builds shards the way an upload writes them. What a *query* returns is different in
	// three ways, all measured against the live CAS: it has a footer, it has no file sections at all, and
	// its chunk hashes are keyed under a key that footer carries. Registered against the file's own chunk
	// hashes, this is the whole cross-repository path minus the network - and it is the check that the
	// first version of this matcher passed every line above and still deduped nothing live, because
	// comparing plain hashes against keyed hashes finds nothing and looks exactly like a cache miss.
	for (auto &test : HFXetStoredShardVectors()) {
		auto name = string("dedup over ") + test.name;
		try {
			auto view = HFXetShardView::Parse(HFXetSelfTestBytesFromHex(test.shard_hex));
			auto chunks = HFXetSelfTestChunks(HFXetTestInputBytes(test.kind, test.size, test.seed, test.pattern));
			vector<HFXetHash> hashes;
			for (auto &chunk : chunks) {
				hashes.push_back(chunk.hash);
			}
			if (hashes.size() < 3 || view.xorbs.empty() || !view.files.empty()) {
				check(name + " fixture is usable", "chunks, a xorb and no file section", "it is not");
				continue;
			}
			HFXetDedupTable table;
			table.Register(view);
			check(name + " registers with no file section", to_string(hashes.size()), to_string(table.ChunkCount()));
			check(name + " knows the file's own chunk hashes", "true", table.Knows(hashes[0]) ? "true" : "false");

			HFXetDedupRun run;
			if (!table.Match(hashes, 0, run)) {
				check(name + " matches", "a run", "nothing");
				continue;
			}
			check(name + " matches the whole xorb", to_string(hashes.size()), to_string(run.count));
			check(name + " names the shard's xorb", view.xorbs[0].hash.Hex(), run.xorb_hash.Hex());
			uint64_t sum_bytes = 0;
			for (auto &chunk : chunks) {
				sum_bytes += chunk.size;
			}
			check(name + " run covers the bytes", to_string(sum_bytes), to_string(run.unpacked_bytes));
			check(name + " run starts at the shard's first chunk", "0", to_string(run.chunk_index_start));

			// a run ends where the hashes stop agreeing, not where the xorb does: starting mid xorb shortens
			// what is left, and a hash that is not in the shard shortens it again
			if (!table.Match(hashes, 2, run) || run.count != hashes.size() - 2 || run.chunk_index_start != 2) {
				check(name + " match starting mid xorb", to_string(hashes.size() - 2) + " at 2",
				      to_string(run.count) + " at " + to_string(run.chunk_index_start));
			} else {
				check(name + " match starting mid xorb", "ok", "ok");
			}
			auto broken = hashes;
			broken[4].bytes[0] = uint8_t(broken[4].bytes[0] ^ 0xFF);
			if (!table.Match(broken, 2, run) || run.count != 2) {
				check(name + " run breaks at a changed hash", "2", to_string(run.count));
			} else {
				check(name + " run breaks at a changed hash", "ok", "ok");
			}

			// Keyed and unkeyed shards side by side is what a table over a long session becomes: one
			// collection per key, each asked in its own terms. The unkeyed one is the same xorb spelled the
			// way an *uploaded* shard spells it - chunk hashes as they are, no key in the footer.
			{
				HFXetShardView unkeyed;
				HFXetShardXorbView plain_xorb;
				plain_xorb.hash = view.xorbs[0].hash;
				plain_xorb.flags = view.xorbs[0].flags;
				uint32_t byte_range = 0;
				for (auto &chunk : chunks) {
					HFXetShardChunkView plain;
					plain.hash = chunk.hash;
					plain.byte_range_start = byte_range;
					plain.unpacked_bytes = uint32_t(chunk.size);
					byte_range += uint32_t(chunk.size);
					plain_xorb.chunks.push_back(plain);
				}
				unkeyed.xorbs.push_back(std::move(plain_xorb));
				check(name + " registers the same content unkeyed as well", to_string(hashes.size()),
				      to_string(table.Register(unkeyed)));
				check(name + " holds one collection per key", "2", to_string(table.KeyCount()));
				check(name + " still matches with both registered", to_string(hashes.size()),
				      table.Match(hashes, 0, run) ? to_string(run.count) : "0");
			}

			// and a shard whose footer key does not match the hashes it carries answers nothing about this
			// file. The reference keys per collection for the same reason the answer has to be per collection:
			// a hash keyed under the wrong key is not a slightly wrong hash, it is a different hash.
			{
				auto rekeyed = HFXetSelfTestBytesFromHex(test.shard_hex);
				// flip the key in the footer, leaving the stored chunk hashes keyed for the old one
				rekeyed[rekeyed.size() - (HF_XET_SHARD_FOOTER_SIZE - HF_XET_SHARD_FOOTER_KEY_OFFSET)] =
				    char(uint8_t(rekeyed[rekeyed.size() - 128] ^ 0xFF));
				HFXetDedupTable elsewhere;
				elsewhere.Register(HFXetShardView::Parse(rekeyed));
				check(name + " keyed differently than its hashes answers nothing", "false",
				      elsewhere.Knows(hashes[0]) ? "true" : "false");
			}
		} catch (std::exception &ex) {
			check(name + " runs", "no exception", ex.what());
		}
	}

	//===----------------------------------------------------------------------===//
	// Which positions are worth a query
	//===----------------------------------------------------------------------===//

	// Eligibility comes from the compiled-in reference answers, so the candidate rules are exercised
	// against hashes xet-core itself called eligible or not - not against hex chosen to make a case.
	vector<HFXetHash> eligible;
	vector<HFXetHash> ineligible;
	for (auto &test : HFXetDedupVectors()) {
		auto hash = HFXetHash::FromHex(test.hash);
		if (test.eligible) {
			eligible.push_back(hash);
		} else {
			ineligible.push_back(hash);
		}
		if (eligible.size() >= 8 && ineligible.size() >= 8) {
			break;
		}
	}
	auto eligible_count = eligible.size();
	auto ineligible_count = ineligible.size();
	// The synthetic files below are built by repeating one ineligible hash, so a handful of each kind is
	// all the candidates need - but a vectors file that had lost one half would let every case below pass
	// for the wrong reason, so both kinds have to actually be present.
	check("dedup candidate vectors have both kinds of hash", "true",
	      eligible_count >= 3 && ineligible_count >= 3 ? "true" : "false");

	HFXetDedupTable empty;
	check("dedup candidates for an empty file", "none", DedupCandidatesText(HFXetDedupCandidates({}, empty)));
	check("dedup candidates ask about the first chunk of a one-chunk file", "0",
	      DedupCandidatesText(HFXetDedupCandidates({ineligible[0]}, empty)));

	vector<HFXetHash> none_eligible(2000, ineligible[0]);
	check("dedup candidates of an unremarkable file are just its first chunk", "0",
	      DedupCandidatesText(HFXetDedupCandidates(none_eligible, empty)));

	// The spacing is 256 chunks, so an eligible chunk 100 in is already covered by the answer the query
	// about chunk 0 got - asking again would be paying for a shard that is very likely the same one.
	auto spaced = none_eligible;
	spaced[100] = eligible[0];
	check("dedup spacing skips an eligible chunk too close to the last query", "0",
	      DedupCandidatesText(HFXetDedupCandidates(spaced, empty)));

	spaced = none_eligible;
	spaced[300] = eligible[0];
	spaced[400] = eligible[1];
	spaced[600] = eligible[2];
	check("dedup candidates follow the spacing but keep asking further out", "0,300,600",
	      DedupCandidatesText(HFXetDedupCandidates(spaced, empty)));

	// A chunk the table already holds is not asked about twice - and unlike a query, knowing it did not
	// arrive with a spacing penalty.
	HFXetDedupTable knows_first;
	HFXetShardView first_only;
	HFXetShardXorbView first_xorb;
	first_xorb.hash = DedupTestHash("xorb of the first chunk");
	offset = 0;
	first_xorb.chunks.push_back(DedupTestChunk("first chunk bytes", offset, 10));
	first_only.xorbs.push_back(std::move(first_xorb));
	knows_first.Register(first_only);
	vector<HFXetHash> file_of_first {first_only.xorbs[0].chunks[0].hash};
	check("dedup does not query for a chunk it already holds", "none",
	      DedupCandidatesText(HFXetDedupCandidates(file_of_first, knows_first)));

	// and with the same first chunk in front of an eligible one 300 out, only the eligible one is asked
	auto held_then = none_eligible;
	held_then[0] = file_of_first[0];
	held_then[300] = eligible[0];
	check("dedup a known first chunk leaves the spacing free for the next candidate", "300",
	      DedupCandidatesText(HFXetDedupCandidates(held_then, knows_first)));

	return failures;
}

} // namespace duckdb
