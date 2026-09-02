#ifndef APP_LOG_H
#define APP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdarg.h>

void APP_LOG_Init(void);
void APP_LOG_Flush(void);
/** Bytes discarded because the console queue was full (0 = none). */
uint32_t APP_LOG_Dropped(void);
int  APP_LOG_Printf(const char *fmt, ...);
void APP_LOG_Write(const char *s);
void APP_LOG_WriteRaw(const uint8_t *data, uint16_t len);
uint8_t APP_LOG_UsbReady(void);
void APP_LOG_SetUsbReady(uint8_t ready);
void APP_LOG_OnTxComplete(void);
/* USB event hooks (see app_log.c): re-arm the console TX path so a bus
 * reset / suspend / resume cycle can never leave the logger stuck busy. */
void APP_LOG_OnUsbConnect(void);
void APP_LOG_OnUsbSuspend(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOG_H */
