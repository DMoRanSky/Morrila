
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include "yyjson.hpp"


namespace duckdb {

using namespace duckdb_yyjson;
static auto json_extract_string_from_keys_(const std::string &s, const std::vector<std::string> &keys) -> std::string {
    /*
    "output": [
        {
        "id": "rs_02b4a860e8e9e0040068dc36905ff881a2bb88d460ba191ca3",
        "type": "reasoning",
        "summary": []
        },
        {
        "id": "msg_02b4a860e8e9e0040068dc36914af481a2b90e16b60c62ffc4",
        "type": "message",
        "status": "completed",
        "content": [
            {
            "type": "output_text",
            "annotations": [],
            "logprobs": [],
            "text": "no"
            }
        ],
        "role": "assistant"
        }
    ]
    then to extract "no", keys = ["output", 1, "content", 0, "text"]
    */
    yyjson_doc *doc = yyjson_read(s.c_str(), s.size(), 0);
    if (!doc) {
        return "";
    }
    yyjson_val *val = yyjson_doc_get_root(doc);
    for (const auto &key : keys) {
        // key can be string or integer (for array index)
        if (yyjson_is_arr(val)) {
            // array index
            int index = std::stoi(key);
            yyjson_val *arr = val;
            if (index < 0 || index >= static_cast<int>(yyjson_arr_size(arr))) {
                yyjson_doc_free(doc);
                return "";
            }
            val = yyjson_arr_get(arr, index);
            continue;
        }
        if (!val || !yyjson_is_obj(val)) {
            yyjson_doc_free(doc);
            return "";
        }
        val = yyjson_obj_get(val, key.c_str());
    }
    std::string result;
    if (val) {
        if (yyjson_is_str(val)) {
            result = std::string(yyjson_get_str(val), yyjson_get_len(val));
        }
        else if (yyjson_is_num(val)){
            // if is a number, convert to string
            result = std::to_string(yyjson_get_int(val));
        }
        else{
            // not string or number
            result = "";
        }
    }
    yyjson_doc_free(doc);
    return result;
}

class LLMLogger {
public:
    void set_log_path(const std::string &path) {
        llm_log_path = path;
    }
    void LLMLog(const std::string &line) {
        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);
        std::ofstream ofs(get_log_path_(), std::ios::app);
        if (!ofs.is_open()){
            return;
        }
        ofs << get_current_timestamp_() << " | " << line << '\n';
    }
private:
    std::string llm_log_path = "";
    std::string get_current_timestamp_() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return std::string(buf);
    }
    const std::string get_log_path_() {
        return llm_log_path.empty() ? "llm_calls.log" : llm_log_path;
    }
};

enum class ReturnType {
    BOOL,
    STRING,
    INT,
    DOUBLE
};

struct LLMResponse {
    std::string text;
    bool success;
    ReturnType return_type = ReturnType::BOOL;
    

    #define DEFAULT_BOOL_RETURN false
    auto parse_to_bool_() -> bool {
        if(!success){
            return DEFAULT_BOOL_RETURN;
        }
        std::string extracted_text = json_extract_string_from_keys_(
            text, {"output", "1", "content", "0", "text"}
        );
        if(extracted_text.empty()){
            return DEFAULT_BOOL_RETURN;
        }
        std::string lowered;
        // Transform extracted_text to lowercase
        std::transform(extracted_text.begin(), extracted_text.end(), std::back_inserter(lowered), [](unsigned char c){ return std::tolower(c); });

        if (lowered == "yes" || lowered == "true") {
            return true;
        } else if (lowered == "no" || lowered == "false") {
            return false;
        }
        return DEFAULT_BOOL_RETURN;
    }

    #define DEFAULT_STRING_RETURN ""
    auto parse_to_string_() -> std::string {
        if(!success){
            return DEFAULT_STRING_RETURN;
        }
        std::string extracted_text = json_extract_string_from_keys_(
            text, {"output", "1", "content", "0", "text"}
        );
        if(extracted_text.empty()){
            return DEFAULT_STRING_RETURN;
        }
        return extracted_text;
    }

    #define DEFAULT_INT_RETURN 0
    auto parse_to_int64_t_() -> int64_t {
        if(!success){
            return DEFAULT_INT_RETURN;
        }
        std::string extracted_text = json_extract_string_from_keys_(
            text, {"output", "1", "content", "0", "text"}
        );
        if(extracted_text.empty()){
            return DEFAULT_INT_RETURN;
        }
        try {
            return std::stoll(extracted_text);
        } catch (const std::invalid_argument& e) {
            return DEFAULT_INT_RETURN;
        } catch (const std::out_of_range& e) {
            return DEFAULT_INT_RETURN;
        }
    }

    #define DEFAULT_DOUBLE_RETURN 0.0
    auto parse_to_double_() -> double {
        if(!success){
            return DEFAULT_DOUBLE_RETURN;
        }
        std::string extracted_text = json_extract_string_from_keys_(
            text, {"output", "1", "content", "0", "text"}
        );
        if(extracted_text.empty()){
            return DEFAULT_DOUBLE_RETURN;
        }
        try {
            return std::stod(extracted_text);
        } catch (const std::invalid_argument& e) {
            return DEFAULT_DOUBLE_RETURN;
        } catch (const std::out_of_range& e) {
            return DEFAULT_DOUBLE_RETURN;
        }
    }

    auto parse() -> std::variant<bool, std::string, int64_t, double> {
        switch(return_type){
            case ReturnType::BOOL:
                return parse_to_bool_();
            case ReturnType::STRING:
                return parse_to_string_();
            case ReturnType::INT:
                return parse_to_int64_t_();
            case ReturnType::DOUBLE:
                return parse_to_double_();
            default:
                //warning: unreachable code
                return parse_to_bool_();
        }
    }
};

struct LLMRequest {
    std::string prompt;
    std::function<void(LLMResponse)> callback;
    LLMRequest() = default;
    LLMRequest(const std::string &p, std::function<void(LLMResponse)> cb) : prompt(p), callback(std::move(cb)) {}
    ReturnType return_type = ReturnType::BOOL;
};

class LLMCallManager {
public:
    explicit LLMCallManager(const std::string &model_name);
    ~LLMCallManager();
    auto send_request(LLMRequest &request) -> void;
    auto get_total_tokens() -> int64_t {
        return total_tokens_.load();
    }
    auto get_total_prompt_tokens() -> int64_t {
        return total_prompt_tokens_.load();
    }
    auto get_total_completion_tokens() -> int64_t {
        return total_completion_tokens_.load();
    }
    auto get_total_cached_tokens() -> int64_t {
        return total_cached_tokens_.load();
    }
    auto get_total_llm_calls() -> int64_t {
        return total_llm_call_.load();
    }
    // expose a thin logging wrapper so external functions can write into the same llm log
    void Log(const std::string &line) {
        logger_.LLMLog(line);
    }
private:
    std::string llm_api_url_;
    std::string model_name_;
    std::string api_key_;
    std::queue<LLMRequest> request_queue_;
    LLMLogger logger_;
    std::atomic<int64_t> total_tokens_{0};
    std::atomic<int64_t> total_prompt_tokens_{0};
    std::atomic<int64_t> total_completion_tokens_{0};
    std::atomic<int64_t> total_cached_tokens_{0};
    std::atomic<int64_t> total_llm_call_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_;

    struct TokenUsage {
        int64_t prompt_tokens = 0;
        int64_t completion_tokens = 0;
        int64_t total_tokens = 0;
        int64_t cached_tokens = 0;
    };

    auto add_tokens_(const TokenUsage& usage) -> void {
        total_tokens_.fetch_add(usage.total_tokens);
        total_prompt_tokens_.fetch_add(usage.prompt_tokens);
        total_completion_tokens_.fetch_add(usage.completion_tokens);
        total_cached_tokens_.fetch_add(usage.cached_tokens);
    }
    auto add_llm_call_() -> void {
        total_llm_call_.fetch_add(1);
    }
    auto workloop_() -> void;
    auto get_response_(const std::string &prompt, ReturnType return_type = ReturnType::BOOL) -> LLMResponse;
    auto construct_body_(const std::string &prompt) -> std::string {
        std::string body = "{\n"
            "  \"model\": \"" + model_name_ + "\",\n"
            "  \"input\": \"";
        static const char *hex = "0123456789ABCDEF";
        for (unsigned char c : prompt) {
            if (c == '\\'){
                body += "\\\\";
            }
            else if (c == '"'){
                body += "\\\"";
            }
            else if (c == '\n'){
                body += "\\n";
            }
            else if (c == '\r'){
                body += "\\r";
            }
            else if (c == '\t'){
                body += "\\t";
            }
            else if (c == '\b'){
                body += "\\b";
            }
            else if (c == '\f'){
                body += "\\f";
            }
            else if (c < 0x20) {
                body += "\\u00";
                body.push_back(hex[(c >> 4) & 0x0F]);
                body.push_back(hex[c & 0x0F]);
            }
            else{
                body.push_back(static_cast<char>(c));
            }
        }
        body += "\"\n";
        body += "}";
        return body;
    }

    auto json_extract_token_usage_(const std::string &s) -> TokenUsage {
        auto safe_stoll = [](const std::string &str) -> int64_t {
            if (str.empty()) {
                return 0;
            }
            try {
                return std::stoll(str);
            } catch (...) {
                return 0;
            }
        };
        TokenUsage usage;
        // Try OpenAI format (prompt_tokens, completion_tokens)
        std::string pt = json_extract_string_from_keys_(s, {"usage", "prompt_tokens"});
        std::string ct = json_extract_string_from_keys_(s, {"usage", "completion_tokens"});
        std::string tt = json_extract_string_from_keys_(s, {"usage", "total_tokens"});
        std::string cat = json_extract_string_from_keys_(s, {"usage", "prompt_tokens_details", "cached_tokens"});

        // Try input_tokens/output_tokens format (seen in logs)
        if (pt.empty() && ct.empty()) {
             pt = json_extract_string_from_keys_(s, {"usage", "input_tokens"});
             ct = json_extract_string_from_keys_(s, {"usage", "output_tokens"});
             if (cat.empty()) {
                 cat = json_extract_string_from_keys_(s, {"usage", "input_tokens_details", "cached_tokens"});
             }
        }

        if (pt.empty() && ct.empty() && tt.empty()) {
            // Try Gemini format
            pt = json_extract_string_from_keys_(s, {"usageMetadata", "promptTokenCount"});
            ct = json_extract_string_from_keys_(s, {"usageMetadata", "candidatesTokenCount"});
            tt = json_extract_string_from_keys_(s, {"usageMetadata", "totalTokenCount"});
            // Gemini cached tokens
            cat = json_extract_string_from_keys_(s, {"usageMetadata", "cachedContentTokenCount"});
        }
        usage.prompt_tokens = safe_stoll(pt);
        usage.completion_tokens = safe_stoll(ct);
        usage.total_tokens = safe_stoll(tt);
        usage.cached_tokens = safe_stoll(cat);
        if (usage.total_tokens == 0 && (usage.prompt_tokens > 0 || usage.completion_tokens > 0)) {
            usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
        }
        return usage;
    };
};

auto batch_check_semantic(const std::vector<std::string> &prompts) -> std::vector<bool>;

auto batch_check_semantic_int(const std::vector<std::string> &prompts) -> std::vector<int64_t>;

auto batch_check_semantic_string(const std::vector<std::string> &prompts) -> std::vector<std::string>;

auto batch_check_semantic_double(const std::vector<std::string> &prompts) -> std::vector<double>;

auto fuse_semantic_filters(const std::string &prompt_1, const std::string &prompt_2, const std::string &model_name = "gpt-5") -> std::string;

// Cache handled internally in llm_query.cpp; no global symbol exported.

} // namespace duckdb