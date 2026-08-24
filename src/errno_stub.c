extern int *__errno_location(void);
int *__errno(void) { return __errno_location(); }
