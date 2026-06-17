#include <stdint.h>

struct imx_uart_regs {
  uint32_t rxd; /* 0x000 Receiver Register */
  uint32_t res0[15];
  uint32_t txd; /* 0x040 Transmitter Register */
  uint32_t res1[15];
  uint32_t cr1;   /* 0x080 Control Register 1 */
  uint32_t cr2;   /* 0x084 Control Register 2 */
  uint32_t cr3;   /* 0x088 Control Register 3 */
  uint32_t cr4;   /* 0x08C Control Register 4 */
  uint32_t fcr;   /* 0x090 FIFO Control Register */
  uint32_t sr1;   /* 0x094 Status Register 1 */
  uint32_t sr2;   /* 0x098 Status Register 2 */
  uint32_t esc;   /* 0x09c Escape Character Register */
  uint32_t tim;   /* 0x0a0 Escape Timer Register */
  uint32_t bir;   /* 0x0a4 BRM Incremental Register */
  uint32_t bmr;   /* 0x0a8 BRM Modulator Register */
  uint32_t brc;   /* 0x0ac Baud Rate Counter Register */
  uint32_t onems; /* 0x0b0 One Millisecond Register */
  uint32_t ts;    /* 0x0b4 Test Register */
};

typedef volatile struct imx_uart_regs imx_uart_regs_t;

int main(void) {
  int i = 0;
  imx_uart_regs_t uart1 = {0};
  return 0;
}