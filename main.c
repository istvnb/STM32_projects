#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#define frequency 8000000 //8MHz ? not sure
#define BIT(x) (1UL << (x))
#define PIN(bank, num) ((((bank) - 'A') << 8) | (num))
#define PINNO(pin) (pin & 255)
#define PINBANK(pin) (pin >> 8)


static inline void spin(volatile uint32_t count) {
  while (count--) asm("nop");
}

struct systick {
  volatile uint32_t CTRL, LOAD, VAL, CALIB;
};
#define SYSTICK ((struct systick *) 0xe000e010)

struct rcc {
  volatile uint32_t CR, CFGR, CIR, APB2RSTR, APB1RSTR, AHBENR,
  APB2ENR, APB1ENR, BDCR, CSR, AHBRSTR, CFGR2, CFGR3, CR2;
};
#define RCC ((struct rcc *) 0x40021000)

static inline void systick_init(uint32_t ticks) {
  if ((ticks - 1) > 0xffffff) return;  // Systick timer is 24 bit
  SYSTICK->LOAD = ticks - 1;
  SYSTICK->VAL = 0;
  SYSTICK->CTRL = BIT(0) | BIT(1) | BIT(2);  // Enable systick
  RCC->APB2ENR |= BIT(0);                   // Enable SYSCFG
}

// Structure for GPIO registers
struct gpio {
  volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2], BRR;
};
#define GPIO(bank) ((struct gpio *) (0x48000000 + 0x400 * (bank)))

// Enum values are per datasheet: 0, 1, 2, 3
enum { GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_AF, GPIO_MODE_ANALOG };


static inline void gpio_set_mode(uint16_t pin, uint8_t mode) {

  //Get the number representing the bank (A-F) and pass it to the GPIO(bank) macro to get the proper pointer for the bank??
  struct gpio *gpio = GPIO(PINBANK(pin));  // GPIO bank
  int n = PINNO(pin);                      // Pin number

  //Enable GPIOx CLock (x = A...F)
  RCC->AHBENR &= ~(BIT(PINBANK(pin)) << 17);
  RCC->AHBENR |= (BIT(PINBANK(pin)) << 17);

  //Set mode
  gpio->MODER &= ~(3U << (n * 2));         // Clear existing setting
  gpio->MODER |= (mode & 3) << (n * 2);    // Set new mode
}

static inline void gpio_set_af(uint16_t pin, uint8_t af_num) {
  struct gpio *gpio = GPIO(PINBANK(pin));  // GPIO bank
  int n = PINNO(pin);                      // Pin number
  gpio->AFR[n >> 3] &= ~(15UL << ((n & 7) * 4));
  gpio->AFR[n >> 3] |= ((uint32_t) af_num) << ((n & 7) * 4);
}

static inline void gpio_write(uint16_t pin, bool val) {
  struct gpio *gpio = GPIO(PINBANK(pin));
  gpio->BSRR = (1U << PINNO(pin)) << (val ? 0 : 16);
}

struct uart {
  volatile uint32_t CR1, CR2, CR3, BRR, GTPR, RTOR, RQR, ISR, ICR, RDR, TDR;
};
#define UART1 ((struct uart *) 0x40013800) //0x40013800
#define UART2 ((struct uart *) 0x40004400)  //0x40004400

static inline void uart_init(struct uart *uart, unsigned long baud) {
  uint8_t af = 1;           // Alternate function
  uint16_t rx = 0, tx = 0;  // pins

  if (uart == UART1) RCC->APB2ENR |= BIT(14);
  if (uart == UART2) RCC->APB1ENR |= BIT(17);

  if (uart == UART1) tx = PIN('A', 9), rx = PIN('A', 10);
  if (uart == UART2) tx = PIN('A', 2), rx = PIN('A', 15);

  gpio_set_mode(tx, GPIO_MODE_AF);
  gpio_set_af(tx, af);
  gpio_set_mode(rx, GPIO_MODE_AF);
  gpio_set_af(rx, af);
  uart->CR1 = 0;                           // Disable this UART
  uart->BRR = frequency / baud;                 // FREQ is a UART bus frequency
  uart->CR1 |= BIT(0) | BIT(2) | BIT(3);  // Set UE, RE, TE
}

static inline int uart_read_ready(struct uart *uart) {
  return uart->ISR & BIT(5);  // If RXNE bit is set, data is ready
}

static inline uint8_t uart_read_byte(struct uart *uart) {
  return (uint8_t) (uart->RDR & 255);
}

static inline void uart_write_byte(struct uart *uart, uint8_t byte) {
  uart->TDR = byte;
  while ((uart->ISR & BIT(7)) == 0) spin(1);
}

static inline void uart_write_buf(struct uart *uart, char *buf, size_t len) {
  while (len-- > 0) uart_write_byte(uart, *(uint8_t *) buf++);
}

static volatile uint32_t s_ticks;
void SysTick_Handler(void) { s_ticks++; }



// t: expiration time, prd: period, now: current time. Return true if expired
bool timer_expired(uint32_t *t, uint32_t prd, uint32_t now) {
  if (now + prd < *t) *t = 0;                    // Time wrapped? Reset timer
  if (*t == 0) *t = now + prd;                   // First poll? Set expiration
  if (*t > now) return false;                    // Not expired yet, return
  *t = (now - *t) > prd ? now + prd : *t + prd;  // Next expiration time
  return true;                                   // Expired, return true
}





int main(void) {

  uint16_t led = PIN('B', 3);
  //Set GPIOB to output
  gpio_set_mode(led, GPIO_MODE_OUTPUT);
  //Set System Tick
  systick_init(frequency / 1000);
  //Set UART
  uart_init(UART2, 9600);

  uint32_t timer = 0, period = 1000;          // Declare timer and 500ms period
  for (;;) {
    if (timer_expired(&timer, period, s_ticks)) {
      static bool on;       // This block is executed
      gpio_write(led, on);  // Every `period` milliseconds
      on = !on;             // Toggle LED state
      uart_write_buf(UART2, "Hi\r\n", 4);  // Write message
      
    }
    // Here we could perform other activities!
  }
  return 0; // Do nothing so far
}



// Startup code
__attribute__((naked, noreturn)) void _reset(void) {
  // memset .bss to zero, and copy .data section to RAM region
  extern long _sbss, _ebss, _sdata, _edata, _sidata;
  for (long *src = &_sbss; src < &_ebss; src++) *src = 0;
  for (long *src = &_sdata, *dst = &_sidata; src < &_edata;) *src++ = *dst++;

  main();             // Call main()
  for (;;) (void) 0;  // Infinite loop in the case if main() returns
}


extern void _estack(void);  // Defined in link.ld

// 16 standard and 32 STM32-specific handlers
__attribute__((section(".vectors"))) void (*tab[16 + 32])(void) = {_estack, _reset , 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, SysTick_Handler};