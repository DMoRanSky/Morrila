#include <bits/stdc++.h>

inline int cnt = 0;
inline int cnt_token = 0;
const double alpha = []() -> double {
    const char* a = std::getenv("ALPHA");
    if (a) return atof(a);
    return 1e7; // default
}();
// LLM cache counters for tests/numa harness (thread-safe)
inline std::atomic<uint64_t> cnt_llm_cache_requests{0};
inline std::atomic<uint64_t> cnt_llm_cache_hits{0};
inline std::string join_left_prompt = "The left has attribute: ";
inline std::string join_right_prompt = "; The right has attribute: ";
inline std::string join_check_prompt = ". Are they related ?";
inline std::string final_prompt = R"("Based on the above information, answer with yes or no. Only yes or no, and no explanation.)";

inline std::string semantic_filter_prompt = "Based on the above information, answer with yes or no. Only yes or no, and no explanation.";

// Global flag to enable/disable semantic optimizer via environment variable.
// Reads SEMANTIC_OPTIMIZER once at load time; defaults to true if unset.
inline bool enable_semantic_optimizer = []() -> bool {
	const char *env = std::getenv("SEMANTIC_OPTIMIZER");
	if (!env) {
		return true; // default
	}
	std::string val(env);
	for (auto &c : val) c = std::tolower((unsigned char)c);
	return (val == "1" || val == "true" || val == "yes" || val == "on");
}();

// Global path for LLM call logs. If left empty, logger will use "llm_calls.log" in CWD.
inline std::string llm_log_path = "";