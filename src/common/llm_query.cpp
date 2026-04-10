#include "duckdb/common/llm_query.hpp"
#include "duckdb/common/unordered_map.hpp"
#include <algorithm>
#include <cpr/cpr.h>
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <future>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

namespace duckdb {

// Hand-written chained hash table for global LLM response cache
class LLMResponseCacheTable {
public:
    struct Node {
        int next = -1;
        std::string key;
        LLMResponse value;
    };

    static constexpr int MOD = 999983;

    struct UpsertResult {
        bool inserted = false;
        int64_t key_len_delta = 0;
        int64_t value_text_len_delta = 0;
    };

    LLMResponseCacheTable() : head_(MOD, -1), bucket_mutex_(MOD) {
        nodes_.reserve(4096);
    }

    auto TryGet(const std::string &key, LLMResponse &out) -> bool {
        const int bucket = Bucket(key);
        std::lock_guard<std::mutex> lk(bucket_mutex_[bucket]);
        for (int i = head_[bucket]; i != -1; i = nodes_[i].next) {
            if (nodes_[i].key == key) {
                out = nodes_[i].value;
                return true;
            }
        }
        return false;
    }

    auto Upsert(const std::string &key, const LLMResponse &value) -> UpsertResult {
        const int bucket = Bucket(key);
        std::lock_guard<std::mutex> lk(bucket_mutex_[bucket]);
        const int64_t new_value_text_len = static_cast<int64_t>(value.text.size());

        for (int i = head_[bucket]; i != -1; i = nodes_[i].next) {
            if (nodes_[i].key == key) {
                const int64_t old_value_text_len = static_cast<int64_t>(nodes_[i].value.text.size());
                nodes_[i].value = value;
                return UpsertResult{false, 0, new_value_text_len - old_value_text_len};
            }
        }

        Node node;
        node.key = key;
        node.value = value;
        node.next = head_[bucket];
        nodes_.push_back(std::move(node));
        head_[bucket] = static_cast<int>(nodes_.size()) - 1;
        return UpsertResult{true, static_cast<int64_t>(key.size()), new_value_text_len};
    }

private:
    std::vector<int> head_;
    std::vector<Node> nodes_;
    std::vector<std::mutex> bucket_mutex_;

    auto Bucket(const std::string &key) const -> int {
        size_t hv = std::hash<std::string>{}(key);
        return static_cast<int>(hv % static_cast<size_t>(MOD));
    }
};

// Global cache for LLM response deduplication: maps composite key -> final response
static LLMResponseCacheTable g_llm_response_cache;
// Global in-flight deduplication across concurrent batches: key -> shared future
static std::mutex g_llm_inflight_mutex;
static std::unordered_map<std::string, std::shared_future<LLMResponse>> g_llm_inflight_futures;
// Global cache metrics
static std::atomic<uint64_t> g_llm_cache_requests{0};
static std::atomic<uint64_t> g_llm_cache_hits{0};
// Approximate cache memory metrics (string payload only)
static std::atomic<int64_t> g_llm_cache_entries{0};
static std::atomic<int64_t> g_llm_cache_key_len_sum{0};
static std::atomic<int64_t> g_llm_cache_value_text_len_sum{0};

auto getLLMCallManagerInstance(const std::string &model_name = "gpt-5-mini") -> LLMCallManager& {
    // Use a leaked singleton per model_name to avoid static destruction order
    // issues where worker threads or mutexes may be accessed during teardown.
    static unordered_map<std::string, LLMCallManager *> instances;
    static std::mutex instances_mutex;
    std::lock_guard<std::mutex> guard(instances_mutex);
    auto it = instances.find(model_name);
    if (it != instances.end()) {
        return *(it->second);
    } else {
        // intentionally allocate and never delete to avoid destructor-order races
        auto *mgr = new LLMCallManager(model_name);
        instances.emplace(model_name, mgr);
        return *mgr;
    }
}

LLMCallManager::LLMCallManager(const std::string &model_name) : model_name_(model_name) {
    // Initialize API key and URL from environment variables or use defaults

    // Set logger path
    logger_.set_log_path("llm_calls_" + model_name_ + ".log");

    api_key_ = std::getenv("API_KEY") ? std::getenv("API_KEY") : "";
    llm_api_url_ = "https://api.openai.com/v1/responses";
    logger_.LLMLog("Initialized LLMCallManager with model: " + model_name_);

    // Initialize token counter
    total_tokens_ = 0;
    total_prompt_tokens_ = 0;
    total_completion_tokens_ = 0;
    total_cached_tokens_ = 0;
    total_llm_call_ = 0;
    
    // Start worker threads
    stop_ = false;
    int num_of_workers = std::getenv("NUM_LLM_WORKERS") ? std::atoi(std::getenv("NUM_LLM_WORKERS")) : 100;
    for (int i = 0; i < num_of_workers; i++) {
        workers_.emplace_back(&LLMCallManager::workloop_, this);
    }
}

LLMCallManager::~LLMCallManager() {
    stop_ = true;
    cv_.notify_all();
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    logger_.LLMLog("LLMCallManager destroyed. Total tokens used: " + std::to_string(total_tokens_.load()) +
                   " (Prompt: " + std::to_string(total_prompt_tokens_.load()) +
                   ", Completion: " + std::to_string(total_completion_tokens_.load()) +
                   ", Cached: " + std::to_string(total_cached_tokens_.load()) + ")" +
                   ", Total LLM calls: " + std::to_string(total_llm_call_.load()));
}

auto LLMCallManager::send_request(LLMRequest &request) -> void{
    std::lock_guard<std::mutex> lock(mutex_);
    request_queue_.push(request);
    cv_.notify_one();
}

auto LLMCallManager::get_response_(const std::string &prompt, ReturnType return_type) -> LLMResponse {
    #define MAX_RETRIES 100
    // Empty/whitespace-only prompts are invalid for Responses API input.
    // Return a failed response directly instead of retrying and eventually throwing.
    if (prompt.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
        logger_.LLMLog("Skip empty prompt request (non-retriable).");
        return LLMResponse{"", false, return_type};
    }

    logger_.LLMLog("PROMPT:\n" + prompt);
    int tries = 0;
    while(true){
        std::string body = construct_body_(prompt);
        cpr::Response r = cpr::Post(
            cpr::Url{llm_api_url_},
            cpr::Header{
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + api_key_}
            },
            cpr::Body{body},
            cpr::Timeout{30000}
        );
        if (r.error.code != cpr::ErrorCode::OK) {
            logger_.LLMLog("HTTP request error: " + r.error.message);
            return LLMResponse{"", false, return_type};
        }
        // successful response
        if (r.status_code >= 200 && r.status_code < 300) {
            // log resource usage
            add_llm_call_();
            TokenUsage usage = json_extract_token_usage_(r.text);
            add_tokens_(usage);
            logger_.LLMLog("Total tokens so far: " + std::to_string(total_tokens_.load()) + 
                           " (P: " + std::to_string(total_prompt_tokens_.load()) +
                           ", C: " + std::to_string(total_completion_tokens_.load()) +
                           ", Cache: " + std::to_string(total_cached_tokens_.load()) + ")" +
                           ", Total LLM calls so far: " + std::to_string(total_llm_call_.load()));
            return LLMResponse{r.text, true, return_type};
        }
        else{
            // 4xx (except 429) are usually request/auth/model issues and should not be retried.
            if (r.status_code >= 400 && r.status_code < 500 && r.status_code != 429) {
                logger_.LLMLog("Non-retriable client error (status code " + std::to_string(r.status_code) + "):\n" + r.text);
                return LLMResponse{"", false, return_type};
            }

            // log error and retry after a delay
            logger_.LLMLog("Error response (status code " + std::to_string(r.status_code) + "):\n" + r.text);
            logger_.LLMLog("Retrying after 1 second...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if(++tries == MAX_RETRIES){
                throw std::runtime_error("Failed to get successful response after 100 retries.");
            }
            continue;
        }
    }
}

auto LLMCallManager::workloop_() -> void {
    while (true) {
        LLMRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !request_queue_.empty() || stop_; });
            if (stop_ && request_queue_.empty()) {
                return;
            }
            request = request_queue_.front();
            request_queue_.pop();
        }
        LLMResponse response = get_response_(request.prompt, request.return_type);
        if (request.callback) {
            request.callback(response);
        }
    }
}

auto batch_fetch_responses(const std::vector<std::pair<std::string, ReturnType>> &prompts, const std::string &model_name = "gpt-5-mini") -> std::vector<LLMResponse> {
    auto &manager = getLLMCallManagerInstance(model_name);
    // per-batch baseline snapshot
    uint64_t batch_req_base = g_llm_cache_requests.load(std::memory_order_relaxed);
    uint64_t batch_hits_base = g_llm_cache_hits.load(std::memory_order_relaxed);
    std::vector<std::shared_future<LLMResponse>> futures;
    // per-batch pending map to dedupe identical prompts within this batch
    std::unordered_map<std::string, std::shared_future<LLMResponse>> pending_futures;

    for (const auto &prompt : prompts) {
        // Build a composite cache key to include return type so different typed requests don't collide
        const std::string cache_key = prompt.first + "\x1f" + std::to_string(static_cast<int>(prompt.second));
        // count this lookup
        g_llm_cache_requests.fetch_add(1, std::memory_order_relaxed);

        // 1) check global cache under lock
        {
            LLMResponse cached_response;
            if (g_llm_response_cache.TryGet(cache_key, cached_response)) {
                g_llm_cache_hits.fetch_add(1, std::memory_order_relaxed);
                std::promise<LLMResponse> p;
                p.set_value(std::move(cached_response));
                futures.push_back(p.get_future().share());
                continue;
            }
        }

        // 1.5) check global in-flight map to dedupe identical prompts across concurrent batches
        {
            std::lock_guard<std::mutex> inflight_lock(g_llm_inflight_mutex);
            auto global_pend = g_llm_inflight_futures.find(cache_key);
            if (global_pend != g_llm_inflight_futures.end()) {
                g_llm_cache_hits.fetch_add(1, std::memory_order_relaxed);
                futures.push_back(global_pend->second);
                continue;
            }
        }

        // 2) check pending_futures for dedupe within this batch
        auto pend = pending_futures.find(cache_key);
        if (pend != pending_futures.end()) {
            // dedupe hit within batch
            g_llm_cache_hits.fetch_add(1, std::memory_order_relaxed);
            futures.push_back(pend->second);
            continue;
        }

        // 3) not found: create new request and store shared_future in pending_futures
        // Create new request for unique prompt
        LLMRequest request;
        request.prompt = prompt.first;
        request.return_type = prompt.second;

        std::promise<LLMResponse> promise;
        auto future = promise.get_future();
        auto shared_future = std::shared_future<LLMResponse>(future.share());
            // store into pending map so duplicates in this batch reuse it
        pending_futures.emplace(cache_key, shared_future);
        futures.push_back(shared_future);

        {
            std::lock_guard<std::mutex> inflight_lock(g_llm_inflight_mutex);
            g_llm_inflight_futures[cache_key] = shared_future;
        }

        auto shared_promise = std::make_shared<std::promise<LLMResponse>>(std::move(promise));
        request.callback = [shared_promise, cache_key](const LLMResponse& response) {
            auto upsert = g_llm_response_cache.Upsert(cache_key, response);
            if (upsert.inserted) {
                g_llm_cache_entries.fetch_add(1, std::memory_order_relaxed);
                g_llm_cache_key_len_sum.fetch_add(upsert.key_len_delta, std::memory_order_relaxed);
            }
            g_llm_cache_value_text_len_sum.fetch_add(upsert.value_text_len_delta, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> inflight_lock(g_llm_inflight_mutex);
                g_llm_inflight_futures.erase(cache_key);
            }
            shared_promise->set_value(response);
        };
        manager.send_request(request);
      
    }

    std::vector<LLMResponse> responses;
    for (auto &future : futures) {
        responses.push_back(future.get());
    }
    // report per-batch delta metrics once
    uint64_t batch_req_now = g_llm_cache_requests.load(std::memory_order_relaxed);
    uint64_t batch_hits_now = g_llm_cache_hits.load(std::memory_order_relaxed);
    double hit_rate = batch_req_now == 0 ? 0.0 : (double)batch_hits_now / (double)batch_req_now;
    const int64_t n = g_llm_cache_entries.load(std::memory_order_relaxed);
    const int64_t key_sum = g_llm_cache_key_len_sum.load(std::memory_order_relaxed);
    const int64_t value_sum = g_llm_cache_value_text_len_sum.load(std::memory_order_relaxed);
    const int64_t sum_len = key_sum + value_sum;
    const long double n_times_sum = static_cast<long double>(n) * static_cast<long double>(sum_len);
    manager.Log("[LLM_CACHE] processed=" + std::to_string(batch_req_now) +
                " hits=" + std::to_string(batch_hits_now) +
                " hit_rate=" + std::to_string(hit_rate) +
                " | entries(n)=" + std::to_string(n) +
                " key_len_sum=" + std::to_string(key_sum) +
                " value_text_len_sum=" + std::to_string(value_sum) +
                " sum_len=" + std::to_string(sum_len) +
                " n_times_sum_len=" + std::to_string(static_cast<unsigned long long>(n_times_sum)));
    return responses;
}

auto batch_check_semantic(const std::vector<std::string> &prompts) -> std::vector<bool> {
    std::vector<std::pair<std::string, ReturnType>> prompt_pairs;
    for (const auto &prompt : prompts) {
        prompt_pairs.emplace_back(prompt, ReturnType::BOOL);
    }
    std::vector<LLMResponse> responses = batch_fetch_responses(prompt_pairs);
    std::vector<bool> results;
    for(auto &response : responses){
        results.push_back(std::get<bool>(response.parse()));
    }
    return results;
}

// naive physical operators
auto batch_check_semantic_int(const std::vector<std::string> &prompts) -> std::vector<int64_t> {
    std::vector<std::pair<std::string, ReturnType>> prompt_pairs;
    for (const auto &prompt : prompts) {
        prompt_pairs.emplace_back(prompt, ReturnType::INT);
    }
    std::vector<LLMResponse> responses = batch_fetch_responses(prompt_pairs);
    std::vector<int64_t> results;
    for(auto &response : responses){
        results.push_back(std::get<int64_t>(response.parse()));
    }
    return results;
}

auto batch_check_semantic_string(const std::vector<std::string> &prompts) -> std::vector<std::string> {
    std::vector<std::pair<std::string, ReturnType>> prompt_pairs;
    for (const auto &prompt : prompts) {
        prompt_pairs.emplace_back(prompt, ReturnType::STRING);
    }
    std::vector<LLMResponse> responses = batch_fetch_responses(prompt_pairs);
    std::vector<std::string> results;
    for(auto &response : responses){
        results.push_back(std::get<std::string>(response.parse()));
    }
    return results;
}

auto batch_check_semantic_double(const std::vector<std::string> &prompts) -> std::vector<double> {
    std::vector<std::pair<std::string, ReturnType>> prompt_pairs;
    for (const auto &prompt : prompts) {
        prompt_pairs.emplace_back(prompt, ReturnType::DOUBLE);
    }
    std::vector<LLMResponse> responses = batch_fetch_responses(prompt_pairs);
    std::vector<double> results;
    for(auto &response : responses){
        results.push_back(std::get<double>(response.parse()));
    }
    return results;
}

auto fuse_semantic_filters(const std::string &prompt_1, const std::string &prompt_2, const std::string &model_name) -> std::string {
    std::string combined_prompt = "Combine the following two semantic filter prompts into one coherent prompt:\nplease only return the combined prompt without any additional explanation.\n Example, if the two prompts are \n check if {table.book} belongs to the fantasy genre.\n and \n check if {table.book} is written by a famous author.\n then the combined prompt should be \n check if {table.book} belongs to the fantasy genre and is written by a famous author.\n";
    combined_prompt += "Prompt 1: " + prompt_1 + "\n";
    combined_prompt += "Prompt 2: " + prompt_2 + "\n";
    combined_prompt += "Combined Prompt:";
    std::vector<std::pair<std::string, ReturnType>> prompt_pairs = {{combined_prompt, ReturnType::STRING}};
    std::vector<LLMResponse> responses = batch_fetch_responses(prompt_pairs, model_name);
    if (!responses.empty() && responses[0].success) {
        return responses[0].parse_to_string_();
    }
    return "";
} 
} // namespace duckdb