#include "ring_log.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <io.h>
using namespace std;

void workThread(uint32_t n) {
	for (uint32_t i = 0; i < n; ++i)
	{
		LOG_INFO("测试日志 %d", i);
	}
}

int main()
{
	thread t;
	LOG_INIT("日志目录", "日志", LOG_TRACE, t);
	t.detach();
	vector<thread> v;
	auto star_tm = chrono::system_clock::now();
	for (int i = 0; i < 4; ++i) {
		v.emplace_back([] {
			workThread(1e7);
			});
	}
	for (int i = 0; i < 4; ++i) {
		v[i].join();
	}
	auto end_tm = chrono::system_clock::now();
	cout << "time use " << chrono::duration_cast<chrono::seconds>(end_tm - star_tm).count() << " s\n";

	return 0;
}
