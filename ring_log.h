#ifndef __RING_LOG_H__
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cassert>

#ifdef WIN32
#include <io.h>
#include <Windows.h>
#include <direct.h>
#define ACCESS _access
#define MKDIR(a) _mkdir((a))
#endif // WIN32


int CreatDir(const char* pszDir);
void be_thdo();

#ifdef WIN32
int gettimeofday(timeval* tp, void* tzp);
#endif // WIN32

#ifdef __linux__
#include <sys/time.h>
#include <cstdarg>
#include <sys/stat.h>
#define MKDIR(a) mkdir((a), 0777)
#endif


enum LOG_LEVEL {
	LOG_FATAL = 1,
	LOG_ERROR,
	LOG_WARN,
	LOG_INFO,
	LOG_DEBUG,
	LOG_TRACE,
};

class utc_time {
public:
	utc_time() {
		timeval tv;
		gettimeofday(&tv, nullptr);
		_sys_acc_sec = tv.tv_sec;
		_sys_acc_min = _sys_acc_sec / 60;
		tm cur_tm;
#ifdef WIN32
		localtime_s(&cur_tm, (const time_t*)& _sys_acc_sec);
#endif // WIN32
		year = cur_tm.tm_year + 1900;
		mon = cur_tm.tm_mon + 1;
		day = cur_tm.tm_mday;
		hour = cur_tm.tm_hour;
		min = cur_tm.tm_min;
		sec = cur_tm.tm_sec;
		reset_utc_fmt();
	}

	uint64_t get_curr_time(int* p_msec = nullptr) {
		timeval tv;
		gettimeofday(&tv, nullptr);
		if (p_msec) {
			*p_msec = tv.tv_sec / 1000;
		}
		//if not in same seconds
		if ((uint64_t)tv.tv_sec != _sys_acc_sec) {
			sec = tv.tv_sec % 60;
			//or if not in same minutes
			if (_sys_acc_sec / 60 != _sys_acc_min) {
				_sys_acc_min = _sys_acc_sec / 60;
				struct tm cur_tm;
#ifdef WIN32
				localtime_s(&cur_tm, (const time_t*)& _sys_acc_sec);
#endif // WIN32
				year = cur_tm.tm_year + 1900;
				mon = cur_tm.tm_mon + 1;
				day = cur_tm.tm_mday;
				hour = cur_tm.tm_hour;
				min = cur_tm.tm_min;
				//reformat utc format
				reset_utc_fmt();
			}
			else {
				//reformat utc format only sec
				reset_utc_fmt_sec();
			}
		}
		return tv.tv_sec;
	}

	int year, mon, day, hour, min, sec;
	char utc_fmt[20];
private:
	void reset_utc_fmt() {
		snprintf(utc_fmt, 20, "%d-%02d-%02d %02d:%02d:%02d", year, mon, day, hour, min, sec);
	}
	void reset_utc_fmt_sec() {
		snprintf(utc_fmt + 17, 3, "%02d", sec);
	}
	uint64_t _sys_acc_min;
	uint64_t _sys_acc_sec;
};

class cell_buffer {
public:
	enum buffer_status
	{
		FREE,
		FULL
	};
	cell_buffer(uint32_t len)
		:status(FREE), next(nullptr),
		_total_len(len), _used_len(0), _data(new char[len]) {
		if (!_data) {
			fprintf(stderr, "no space to allocate _data\n");
			exit(1);
		}
	}

	uint32_t avail_len() const { return _total_len - _used_len; }
	bool empty() const { return _used_len == 0; }

	void append(const char* log_line, uint32_t len) {
		if (avail_len() < len)
			return;
		memcpy(_data + _used_len, log_line, len);
		_used_len += len;
	}
	void clear() {
		_used_len = 0;
		status = FREE;
	}
	void persist(FILE* fp) {
		uint32_t wt_len = fwrite(_data, 1, _used_len, fp);
		if (wt_len != _used_len) {
			fprintf(stderr, "write log to disk error, wt_len %u\n", wt_len);
		}
	}

	buffer_status status;
	cell_buffer* next;
private:
	cell_buffer(const cell_buffer&) = delete;
	cell_buffer& operator=(const cell_buffer&) = delete;

	uint32_t _total_len;
	uint32_t _used_len;
	char* _data;
};

class ring_log {
public:
	static ring_log* ins() {
		std::call_once(_once, ring_log::init);
		return _ins;
	}
	static void init() {
		if (!_ins)
			_ins = new ring_log();
	}

	void init_path(const char* log_dir, const char* prog_name, LOG_LEVEL level);
	int get_level()const { return  _level; }

	void persist();
	void try_append(const char* lvl, const char* format, ...);

private:
	ring_log();
	bool decis_file(int year, int mon, int day);
	ring_log(const ring_log&) = delete;
	ring_log& operator=(const ring_log&) = delete;

	int _buff_cnt;
	cell_buffer* _curr_buf;
	cell_buffer* _prst_buf;
	cell_buffer* last_buf;
	static uint32_t _one_buf_len;

	FILE* _fp;
	int _year, _mon, _day, _log_cnt;
	char _prog_name[128];
	char _log_dir[512];
	bool _env_ok;//if log dir ok

	LOG_LEVEL _level;
	uint64_t _lst_lts;

	utc_time _tm;

	std::mutex _mutex;
	std::condition_variable _cond;
	static ring_log* _ins;
	static std::once_flag _once;

	std::thread::id _pid;
};

//format: [LEVEL][yy-mm-dd h:m:s.ms][tid]file_name:line_no(func_name):content

#ifdef WIN32
void LOG_INIT(const char* log_dir, const char* prog_name, LOG_LEVEL level, std::thread& t);

#define LOG_TRACE(fmt, args, ...) \
    if (ring_log::ins()->get_level() >= LOG_TRACE) \
        { \
            ring_log::ins()->try_append("[TRACE]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        }

#define LOG_DEBUG(fmt, args, ...) \
    if (ring_log::ins()->get_level() >= LOG_DEBUG) \
        { \
            ring_log::ins()->try_append("[DEBUG]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        } \

#define LOG_INFO(fmt, args, ...) \
    if (ring_log::ins()->get_level() >= LOG_INFO) \
        { \
            ring_log::ins()->try_append("[INFO]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        } \

#define LOG_WARN(fmt, args, ...) \
    if (ring_log::ins()->get_level() >= LOG_WARN) \
        { \
            ring_log::ins()->try_append("[WARN]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        } \

#define LOG_ERROR(fmt, args, ...) \
    if (ring_log::ins()->get_level() >= LOG_ERROR) \
        { \
            ring_log::ins()->try_append("[ERROR]", "[%u]%s:%d(%s): " fmt "\n", \
                std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        }

#define LOG_FATAL(fmt, args, ...) \
    ring_log::ins()->try_append("[FATAL]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args);
#endif // WIN32

#ifdef __linux__
void LOG_INIT(const char* log_dir, const char* prog_name, LOG_LEVEL level, std::thread& t);

#define LOG_TRACE(fmt, args...) \
    if (ring_log::ins()->get_level() >= LOG_TRACE) \
        { \
            ring_log::ins()->try_append("[TRACE]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        }

#define LOG_DEBUG(fmt, args...) \
    if (ring_log::ins()->get_level() >= LOG_DEBUG) \
        { \
            ring_log::ins()->try_append("[DEBUG]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        } \

#define LOG_INFO(fmt, args...) \
    if (ring_log::ins()->get_level() >= LOG_INFO) \
        { \
            ring_log::ins()->try_append("[INFO]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        } \

#define LOG_WARN(fmt, args...) \
    if (ring_log::ins()->get_level() >= LOG_WARN) \
        { \
            ring_log::ins()->try_append("[WARN]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        } \

#define LOG_ERROR(fmt, args...) \
    if (ring_log::ins()->get_level() >= LOG_ERROR) \
        { \
            ring_log::ins()->try_append("[ERROR]", "[%u]%s:%d(%s): " fmt "\n", \
                std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args); \
        }

#define LOG_FATAL(fmt, args...) \
    ring_log::ins()->try_append("[FATAL]", "[%u]%s:%d(%s): " fmt "\n", \
               std::this_thread::get_id(), __FILE__, __LINE__, __FUNCTION__, ##args);
#endif // WIN32

#endif // !__RING_LOG_H__
