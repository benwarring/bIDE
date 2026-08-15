#ifndef RENDER_H
#define RENDER_H

void editor_scroll(void);

void editor_refresh_screen(void);

void editor_set_status_message(const char *fmt, ...);

void render_free(void);

#endif
