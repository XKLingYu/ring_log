#include "ring_log.h"
#ifdef WIN32
int gettimeofday(timeval* tp, void* tzp) {
	time_t clock;
	tm t;
	SYSTEMTIME wtm;
	GetLocalTime(&wtm);
	t.tm_year = wtm.wYear - 1900;
	t.tm_mon = wtm.wMonth - 1;
	t.tm_mday = wtm.wDay;
	t.tm_hour = wtm.wHour;
	t.tm_min = wtm.wMinute;
	t.tm_sec = wtm.wSecond;
	t.tm_isdst = -1;
	clock = mktime(&t);
	tp->tv_sec = clock;
	tp->tv_usec = wtm.wMilliseconds * 1000;
	return 0;
}
#endif // WIN32

int CreatDir(const char* pszDir) {
	MKDIR(pszDir);
	return 0;
}

void be_thdo() {
	ring_log::ins()->persist();
}

const static uint32_t MEM_USE_LIMIT = (3u * 1024 * 1024 * 1024);//3GB
const static uint32_t LOG_USE_LIMIT = (1u * 1024 * 1024 * 1024);//1GB
const static uint32_t LOG_LEN_LIMIT = (4 * 1024);//4K
uint32_t ring_log::_one_buf_len = 30 * 1024 * 1024;
const static int RELOG_THRESOLD = 5;
const static int BUFF_WAIT_TIME = 1;
ring_log* ring_log::_ins = nullptr;
std::once_flag ring_log::_once;

ring_log::ring_log()
	:_buff_cnt(3), _curr_buf(nullptr),
	_prst_buf(nullptr), _fp(nullptr),
	_log_cnt(0), _env_ok(false),
	_level(LOG_INFO),
	_lst_lts(0), _tm()
{
	cell_buffer* head = new cell_buffer(_one_buf_len);
	if (!head) {
		fprintf(stderr, "no space to allocate cell_buffer\n");
		exit(1);
	}
	cell_buffer* curr_buf;
	cell_buffer* prev = head;
	for (int i = 1; i < _buff_cnt; ++i) {
		curr_buf = new cell_buffer(_one_buf_len);
		if (!curr_buf) {
			fprintf(stderr, "no space to allocate cell_buffer\n");
			exit(1);
		}
		prev->next = curr_buf;
		prev = curr_buf;
	}
	
	prev->next = head;
	_curr_buf = head;
	_prst_buf = head;
	_pid = std::this_thread::get_id();
}

void ring_log::init_path(const char* log_dir, const char* prog_name, LOG_LEVEL level) {
	std::lock_guard<std::mutex> lock(_mutex);
	strncpy(_log_dir, log_dir, strlen(log_dir));
	strncpy(_prog_name, prog_name, strlen(prog_name));
	CreatDir(_log_dir);
	_env_ok = true;

	if (level > LOG_TRACE)
		level = LOG_TRACE;
	if (level < LOG_FATAL)
		level = LOG_FATAL;

	_level = level;
}

void ring_log::persist() {
	while (true) {
		std::unique_lock<std::mutex> lock(_mutex);
		if (_prst_buf->status == cell_buffer::FREE) {
			_cond.wait_for(lock, std::chrono::seconds(1));
		}
		if (_prst_buf->empty()) {
			continue;
		}
		if (_prst_buf->status == cell_buffer::FREE) {
			assert(_curr_buf == _prst_buf);//to test
			_curr_buf->status = cell_buffer::FULL;
			_curr_buf = _curr_buf->next;
		}

		int year = _tm.year, mon = _tm.mon, day = _tm.day;
		lock.unlock();

		if (!decis_file(year, mon, day))
			continue;
		_prst_buf->persist(_fp);
		fflush(_fp);

		lock.lock();
		_prst_buf->clear();
		_prst_buf = _prst_buf->next;
	}
}

void ring_log::try_append(const char* lvl, const char* format, ...) {
	int ms;
	uint64_t curr_sec = _tm.get_curr_time(&ms);
	if (_lst_lts && curr_sec - _lst_lts < RELOG_THRESOLD)
		return;

	char log_line[LOG_LEN_LIMIT];
	int prev_len = snprintf(log_line, LOG_LEN_LIMIT, "%s[%s.%03d]", lvl, _tm.utc_fmt, ms);

	va_list arg_list;
	va_start(arg_list, format);
	int main_len = vsnprintf(log_line + prev_len, LOG_LEN_LIMIT - prev_len, format, arg_list);
	va_end(arg_list);

	int len = prev_len + main_len;
	_lst_lts = 0;
	bool tell_back = false;

	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_curr_buf->status == cell_buffer::FREE && _curr_buf->avail_len() >= len) {
			_curr_buf->append(log_line, len);
		}
		else {
			if (_curr_buf->status == cell_buffer::FREE) {
				_curr_buf->status = cell_buffer::FULL;
				cell_buffer* next_buf = _curr_buf->next;
				tell_back = true;

				if (next_buf->status == cell_buffer::FULL) {
					if (_one_buf_len * (_buff_cnt + 1) > MEM_USE_LIMIT) {
						fprintf(stderr, "no more log space can use\n");
						_curr_buf = next_buf;
						_lst_lts = curr_sec;
					}
					else {
						cell_buffer* new_buffer = new cell_buffer(_one_buf_len);
						_buff_cnt += 1;
						_curr_buf->next = new_buffer;
						new_buffer->next = next_buf;
						_curr_buf = new_buffer;
					}
				}
				else {
					_curr_buf = next_buf;
				}
				if (!_lst_lts) {
					_curr_buf->append(log_line, len);
				}
			}
			else {
				fprintf(stderr, "_curr_buf == _prst_buf\n");
				_lst_lts = curr_sec;
			}
		}
	}
	if (tell_back) {
		_cond.notify_one();
	}
}

bool ring_log::decis_file(int year, int mon, int day) {
	if (!_env_ok) {
		return false;
	}
	char log_path[1024] = {};
	if (!_fp) {
		_year = year, _mon = mon, _day = day;
		sprintf(log_path, "%s/%s.%d%02d%02d.%u.log", _log_dir, _prog_name, _year, _mon, _day, _pid);
		_fp = fopen(log_path, "w");
		if (_fp) {
			_log_cnt += 1;
		}
	}
	else if (_day != day) {
		fclose(_fp);
		_year = year, _mon = mon, _day = day;
		sprintf(log_path, "%s/%s.%d%02d%02d.%u.log", _log_dir, _prog_name, _year, _mon, _day, _pid);
		_fp = fopen(log_path, "w");
		if (_fp) {
			_log_cnt = 1;
		}
	}
	else if (ftell(_fp) >= LOG_USE_LIMIT) {
		fclose(_fp);
		char old_path[1024] = {};
		char new_path[1024] = {};
		for (int i = _log_cnt - 1; i > 0; --i)
		{
			sprintf(old_path, "%s/%s.%d%02d%02d.%u.log.%d", _log_dir, _prog_name, _year, _mon, _day, _pid, i);
			sprintf(new_path, "%s/%s.%d%02d%02d.%u.log.%d", _log_dir, _prog_name, _year, _mon, _day, _pid, i + 1);
			rename(old_path, new_path);
		}
		sprintf(old_path, "%s/%s.%d%02d%02d.%u.log", _log_dir, _prog_name, _year, _mon, _day, _pid);
		sprintf(new_path, "%s/%s.%d%02d%02d.%u.log.1", _log_dir, _prog_name, _year, _mon, _day, _pid);
		rename(old_path, new_path);
		_fp = fopen(old_path, "w");
		if (_fp)
			_log_cnt += 1;
	}
	return _fp != nullptr;
}


void LOG_INIT(const char* log_dir, const char* prog_name, LOG_LEVEL level, std::thread& t) {
	ring_log::ins()->init_path(log_dir, prog_name, level);
	t = std::thread(be_thdo);
}
