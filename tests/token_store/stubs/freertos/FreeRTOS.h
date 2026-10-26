#pragma once

/* host 单测替身:把临界区映射成 pthread 互斥锁。
 *
 * 刻意不用空实现 —— 空实现会让"锁内比较/锁内写入"的失效在并发用例里
 * 完全看不出来(撕裂 bug 就那么漏过去了)。用真锁才能验证时序假设。 */
#include <pthread.h>

typedef pthread_mutex_t portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux)      pthread_mutex_lock(mux)
#define portEXIT_CRITICAL(mux)       pthread_mutex_unlock(mux)
