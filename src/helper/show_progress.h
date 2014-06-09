#ifndef OPENOCD_HELPER_SHOW_PROGRESS_H
#define OPENOCD_HELPER_SHOW_PROGRESS_H

int show_progress(long long now);
void init_progression_bar(int max);
void stop_progression_bar(void);

#endif /* OPENOCD_HELPER_SHOW_PROGRESS_H */
