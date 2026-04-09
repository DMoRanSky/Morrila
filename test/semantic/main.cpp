#include "duckdb.hpp"

#include "numa_config.hpp"

// #include "ittnotify.h"

#include <fstream>
#include <iostream>
#include <filesystem>
#include <sys/time.h>

#include <chrono>

double GetNow() {
    using namespace std::chrono;
    auto now = high_resolution_clock::now();
    return duration<double>(now.time_since_epoch()).count();  // 秒为单位
}

using namespace duckdb;

double RunQuery(Connection &con, std::string query) {
	auto query_start = GetNow();
	auto result = con.Query(query);
	auto query_end = GetNow();
	return query_end - query_start;
}

std::string Query(std::string);

int main(int argc, char *argv[]) {
    std::filesystem::path sql_text_dict("../");
    std::filesystem::directory_iterator sql_text_dict_list(sql_text_dict);
    std::vector<std::string> sqls;
    for (auto &sql_file : sql_text_dict_list) {
        auto sql_name = sql_file.path().filename().string();
        if (sql_file.path().extension() == ".sql") {
            sqls.push_back(sql_name.substr(0, sql_name.size() - 4));
        }
    }
    std::sort(sqls.begin(), sqls.end(), [](const std::string &a, const std::string &b) {
        if (a.length() != b.length()) {
            return a.length() < b.length();
        }
        return a < b;
    });

	

	for (auto sql_name : sqls) {
        cnt = 0; 
        cnt_token = 0;
        std::cout << "Running SQL: " << sql_name << std::endl;
        std::string sql_text = Query(sql_name);  // 读取 SQL 文件
        std::cout << "SQL text:" << std::endl;
        std::cout << sql_text << std::endl;
    
        DuckDB db("imdb.db");        // 新建数据库对象
        Connection con(db);        // 建立连接
        con.Query("SET threads TO 96;");
        con.Query("set enable_profiling='json';");
    
        auto query_start = GetNow();
        auto result = con.Query(sql_text);
        auto query_end = GetNow();
    
        //result->Print();
        std::cout << "LLM (Gemini) API Invoked time: " << cnt << std::endl;
        std::cout << "Token: " << cnt_token << std::endl;
        std::cout << "Time: " << (query_end - query_start) << " s" << std::endl;
    }
}

std::string Query(std::string number) {
	std::ifstream sqlfile("../" + number + ".sql");
	return std::string((std::istreambuf_iterator<char>(sqlfile)), std::istreambuf_iterator<char>());
}