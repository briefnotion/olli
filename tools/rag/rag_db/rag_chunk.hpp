// chunk_text - paragraph-aware text splitter for RAG ingestion. Splits on
// blank-line paragraph boundaries, then greedily packs paragraphs into
// ~chunk_words-word chunks, carrying overlap_words of trailing overlap into
// the next chunk so a concept split across a boundary is still findable via
// either neighbor's embedding.

#pragma once

#include <string>
#include <vector>

// "Words" here means whitespace-separated tokens, not real LLM tokens - an
// approximation that avoids pulling in a tokenizer dependency; close enough
// at chunk-size granularity. A single paragraph longer than chunk_words on
// its own still becomes one (oversized) chunk - splitting mid-paragraph
// would need word-level packing instead, not worth the complexity until
// real content actually hits this case.
//
// Default dropped from an original 400/40 to 150/20 (2026-09-08) - on a
// dense, jargon-heavy technical doc, 400-word chunks were routinely merging
// two or three unrelated subsections together, diluting each chunk's
// embedding enough that the chunk actually containing a test query's answer
// ranked near the bottom of a 9-chunk document instead of near the top.
// Smaller chunks keep each markdown subsection (a natural semantic unit in
// most imported content) closer to its own chunk.
std::vector<std::string> chunk_text(const std::string& text, int chunk_words = 150, int overlap_words = 20);

// True if a chunk is dominated by a small number of distinct words repeated
// many times - found live in a real chat_log (a remote-tool error message,
// logged, then repeated near-verbatim several times before the next real
// exchange): such a chunk carries almost no real signal, but its embedding
// still scored anomalously well against unrelated queries, outranking a
// genuinely on-topic document for a real search ("automotive" scored 0.62
// on a 4x-repeated error message vs. 0.56 on the actual on-topic note).
// Threshold picked from real data: legitimate notes/conversation chunks
// measured 0.58-0.94 distinct-word ratio; the offending chunk measured
// 0.26. The word-count floor avoids flagging a short chunk where a low
// ratio is just sampling noise on few words, not real repetition.
bool is_low_information_chunk(const std::string& text);
