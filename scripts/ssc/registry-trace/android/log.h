/* Host-only liblog boundary; fixture captures the actual function's message. */
#define ANDROID_LOG_INFO 4
int __android_log_print(int priority, const char *tag, const char *format, ...);
