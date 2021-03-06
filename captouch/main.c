#include <libopencm3/cm3/common.h>
#include <libopencm3/cm3/vector.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>
#include <libopencm3/efm32/cmu.h>
#include <libopencm3/efm32/gpio.h>
#include <libopencm3/efm32/common/prs_common.h>
#include <libopencm3/efm32/common/acmp_common.h>
#include <libopencm3/efm32/timer.h>
#include <libopencm3/efm32/wdog.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "captouch.h"
#include "capsenseconfig.h"
#include "printf.h"

// Make this program compatible with Toboot-V2.0
#include <toboot.h>
TOBOOT_CONFIGURATION(0);

#define LED_GREEN_PORT GPIOA
#define LED_GREEN_PIN GPIO0
#define LED_RED_PORT GPIOB
#define LED_RED_PIN GPIO7
#define CAP0B_PORT GPIOE
#define CAP0B_PIN GPIO12
#define CAP1B_PORT GPIOE
#define CAP1B_PIN GPIO13

#pragma warning "Re-defining TIMER_CC_CTRL_INSEL because it's wrong"
#undef TIMER_CC_CTRL_INSEL
#define TIMER_CC_CTRL_INSEL (1 << 20)

#define EFM_ASSERT(x)

extern void usb_setup(void);
extern void usb_puts(const char *s);
void udelay_busy(uint32_t usecs);

/**************************************************************************//**
 * @brief This vector stores the latest read values from the ACMP
 * @param ACMP_CHANNELS Vector of channels.
 *****************************************************************************/
static volatile uint32_t g_channel_values[4] = {0};

/**************************************************************************//**
 * @brief  This stores the maximum values seen by a channel
 * @param ACMP_CHANNELS Vector of channels.
 *****************************************************************************/
static volatile uint32_t channelMaxValues[4] = {0};

/** The current channel we are sensing. */
static volatile uint8_t g_current_channel;

/** Which generation of capsense we're on.  Monotonically increasing. */
static volatile uint32_t g_capsense_generation;

/** Set to true when we're freerunning capsense */
static volatile bool g_capsense_running = false;

/***************************************************************************//**
 * @brief
 *   Sets the ACMP channel used for capacative sensing.
 *
 * @note
 *   A basic example of capacative sensing can be found in the STK BSP
 *   (capsense demo).
 *
 * @param[in] acmp
 *   Pointer to ACMP peripheral register block.
 *
 * @param[in] channel
 *   The ACMP channel to use for capacative sensing (Possel).
 ******************************************************************************/
static void ACMP_CapsenseChannelSet(uint32_t channel)
{
    g_current_channel = channel;

    if (channel == 0) {
        MMIO32(ACMP0_INPUTSEL) = (acmpResistor0 << _ACMP_INPUTSEL_CSRESSEL_SHIFT)
                            | ACMP_INPUTSEL_CSRESEN
                            | (false << _ACMP_INPUTSEL_LPREF_SHIFT)
                            | (0x3f << _ACMP_INPUTSEL_VDDLEVEL_SHIFT)
                            | ACMP_INPUTSEL_NEGSEL(ACMP_INPUTSEL_NEGSEL_CAPSENSE)
                            | (channel << _ACMP_INPUTSEL_POSSEL_SHIFT);
    }
    else if (channel == 1) {
        MMIO32(ACMP0_INPUTSEL) = (acmpResistor0 << _ACMP_INPUTSEL_CSRESSEL_SHIFT)
                        | ACMP_INPUTSEL_CSRESEN
                        | (false << _ACMP_INPUTSEL_LPREF_SHIFT)
                        | (0x3d << _ACMP_INPUTSEL_VDDLEVEL_SHIFT)
                        | ACMP_INPUTSEL_NEGSEL(ACMP_INPUTSEL_NEGSEL_CAPSENSE)
                        | (channel << _ACMP_INPUTSEL_POSSEL_SHIFT);
    }
    else if (channel == 2)
        ;
    else if (channel == 3)
        ;
    else
        while(1);
}

/**************************************************************************//**
 * @brief
 *   Start a capsense measurement of a specific channel and waits for
 *   it to complete.
 *****************************************************************************/
static void CAPSENSE_Measure(uint32_t channel)
{
    /* Set up this channel in the ACMP. */
    ACMP_CapsenseChannelSet(channel);

    /* Reset timers */
    TIMER0_CNT = 0;
    TIMER1_CNT = 0;

    /* Start timers */
    TIMER0_CMD = TIMER_CMD_START;
    TIMER1_CMD = TIMER_CMD_START;

    if (channel == 2) {
        gpio_mode_setup(CAP0B_PORT, GPIO_MODE_PUSH_PULL, CAP0B_PIN);
        gpio_set(CAP0B_PORT, CAP0B_PIN);
        gpio_mode_setup(CAP0B_PORT, GPIO_MODE_INPUT, CAP0B_PIN);
        while (gpio_get(CAP0B_PORT, CAP0B_PIN) && (TIMER0_CNT < (TIMER0_TOP - 5)))
            ;
        g_channel_values[channel] = TIMER0_CNT;
    }
    else if (channel == 3) {
        gpio_mode_setup(CAP1B_PORT, GPIO_MODE_PUSH_PULL, CAP1B_PIN);
        gpio_set(CAP1B_PORT, CAP1B_PIN);
        gpio_mode_setup(CAP1B_PORT, GPIO_MODE_INPUT, CAP1B_PIN);
        while (gpio_get(CAP1B_PORT, CAP1B_PIN) && (TIMER0_CNT < (TIMER0_TOP - 5)))
            ;
        g_channel_values[channel] = TIMER0_CNT;
    }
}


/**************************************************************************//**
 * @brief
 *   TIMER0 interrupt handler.
 *
 * @detail
 *   When TIMER0 expires the number of pulses on TIMER1 is inserted into
 *   channelValues. If this values is bigger than what is recorded in
 *   channelMaxValues, channelMaxValues is updated.
 *   Finally, the next ACMP channel is selected.
 *****************************************************************************/
void timer0_isr(void)
{
  uint32_t count;

  /* Stop timers */
  TIMER0_CMD = TIMER_CMD_STOP;
  TIMER1_CMD = TIMER_CMD_STOP;

  /* Clear interrupt flag */
  TIMER0_IFC = TIMER_IFC_OF;

  /* Read out value of TIMER1 */
  count = TIMER1_CNT;

  /* Store value in channelValues */
  g_channel_values[g_current_channel] = count;

  /* Update channelMaxValues */
  if (count > channelMaxValues[g_current_channel])
    channelMaxValues[g_current_channel] = count;

  if (g_capsense_running) {
      if (g_current_channel >= 3) {
          g_capsense_generation++;
          g_current_channel = 0;
      }
      else {
          g_current_channel++;
      }
      CAPSENSE_Measure(g_current_channel);
  }
  else {
      /* Disable the ACMP, since capsense is no longer running */
      MMIO32(ACMP0_CTRL) &= ~ACMP_CTRL_EN;
  }
}

void capsense_start(void) {
    g_capsense_running = true;

    /* Set the "Enable" Bit in ACMP, so we can make analog measurements */
    MMIO32(ACMP0_CTRL) |= ACMP_CTRL_EN;

    CAPSENSE_Measure(0);
}

void capsense_stop(void) {
    g_capsense_running = false;
}

/***************************************************************************/ /**
 * @brief
 *   Sets up the ACMP for use in capacative sense applications.
 *
 * @details
 *   This function sets up the ACMP for use in capacacitve sense applications.
 *   To use the capacative sense functionality in the ACMP you need to use
 *   the PRS output of the ACMP module to count the number of oscillations
 *   in the capacative sense circuit (possibly using a TIMER).
 *
 * @note
 *   A basic example of capacative sensing can be found in the STK BSP
 *   (capsense demo).
 *
 * @param[in] acmp
 *   Pointer to ACMP peripheral register block.
 *
 * @param[in] init
 *   Pointer to initialization structure used to configure ACMP for capacative
 *   sensing operation.
 ******************************************************************************/

void setup_acmp_capsense(const struct acmp_capsense_init *init)
{
    /* Make sure the module exists on the selected chip */
    EFM_ASSERT(ACMP_REF_VALID(acmp));

    /* Make sure that vddLevel is within bounds */
    EFM_ASSERT(init->vddLevel < 64);

    /* Make sure biasprog is within bounds */
    EFM_ASSERT(init->biasProg <=
               (_ACMP_CTRL_BIASPROG_MASK >> _ACMP_CTRL_BIASPROG_SHIFT));

    /* Set control register. No need to set interrupt modes */
    MMIO32(ACMP0_CTRL) = (init->fullBias << _ACMP_CTRL_FULLBIAS_SHIFT)
                        | (init->halfBias << _ACMP_CTRL_HALFBIAS_SHIFT)
                        | (init->biasProg << _ACMP_CTRL_BIASPROG_SHIFT)
                        | (init->warmTime << _ACMP_CTRL_WARMTIME_SHIFT)
                        | (init->hysteresisLevel << _ACMP_CTRL_HYSTSEL_SHIFT)
        ;

    /* Select capacative sensing mode by selecting a resistor and enabling it */
    MMIO32(ACMP0_INPUTSEL) = (init->resistor << _ACMP_INPUTSEL_CSRESSEL_SHIFT)
                        | ACMP_INPUTSEL_CSRESEN
                        | (init->lowPowerReferenceEnabled << _ACMP_INPUTSEL_LPREF_SHIFT)
                        | (init->vddLevel << _ACMP_INPUTSEL_VDDLEVEL_SHIFT)
                        | ACMP_INPUTSEL_NEGSEL(ACMP_INPUTSEL_NEGSEL_CAPSENSE)
        ;

    /* Enable ACMP if requested. */
    if (init->enable)
        MMIO32(ACMP0_CTRL) |= (1 << _ACMP_CTRL_EN_SHIFT);
}

static void setup_capsense(void)
{
    const struct acmp_capsense_init capsenseInit = ACMP_CAPSENSE_INIT_DEFAULT;
    CMU_HFPERCLKDIV |= CMU_HFPERCLKDIV_HFPERCLKEN;
    //cmu_periph_clock_enable(CMU_HFPER);
    cmu_periph_clock_enable(CMU_TIMER0);
    cmu_periph_clock_enable(CMU_TIMER1);

    CMU_HFPERCLKEN0 |= ACMP_CAPSENSE_CLKEN;
    cmu_periph_clock_enable(CMU_PRS);

    /* Initialize TIMER0 - Prescaler 2^9, top value 10, interrupt on overflow */
    TIMER0_CTRL = TIMER_CTRL_PRESC(TIMER_CTRL_PRESC_DIV512);
    TIMER0_TOP = 10;
    TIMER0_IEN = TIMER_IEN_OF;
    TIMER0_CNT = 0;

    /* Initialize TIMER1 - Prescaler 2^10, clock source CC1, top value 0xFFFF */
    TIMER1_CTRL = TIMER_CTRL_PRESC(TIMER_CTRL_PRESC_DIV1024) | TIMER_CTRL_CLKSEL(TIMER_CTRL_CLKSEL_CC1);
    TIMER1_TOP  = 0xFFFF;

    /* Set up TIMER1 CC1 to trigger on PRS channel 0 */
    TIMER1_CC1_CTRL = TIMER_CC_CTRL_MODE(TIMER_CC_CTRL_MODE_INPUTCAPTURE) /* Input capture      */
                        | TIMER_CC_CTRL_PRSSEL(TIMER_CC_CTRL_PRSSEL_PRSCH0)   /* PRS channel 0      */
                        | TIMER_CC_CTRL_INSEL           /* PRS input selected */
                        | TIMER_CC_CTRL_ICEVCTRL(TIMER_CC_CTRL_ICEVCTRL_RISING) /* PRS on rising edge */
                        | TIMER_CC_CTRL_ICEDGE(TIMER_CC_CTRL_ICEDGE_BOTH);    /* PRS on rising edge */

    /*Set up PRS channel 0 to trigger on ACMP0 output*/
    PRS_CH0_CTRL = PRS_CH_CTRL_EDSEL_POSEDGE              /* Posedge triggers action */
                   | PRS_CH_CTRL_SOURCESEL(PRS_CH_CTRL_SOURCESEL_ACMP_CAPSENSE)  /* PRS source */
                   | PRS_CH_CTRL_SIGSEL(PRS_CH_CTRL_SIGSEL_ACMPOUT_CAPSENSE); /* PRS signal */

    /* Set up ACMP0 in capsense mode */
    setup_acmp_capsense(&capsenseInit);

    /* Enable TIMER0 interrupt */
    nvic_enable_irq(NVIC_TIMER0_IRQ);
}

static void setup(void)
{
    /* GPIO peripheral clock is necessary for us to set up the GPIO pins as outputs */
    cmu_periph_clock_enable(CMU_GPIO);

    /* Set up both LEDs as outputs */
    gpio_mode_setup(LED_RED_PORT, GPIO_MODE_WIRED_AND, LED_RED_PIN);
    gpio_mode_setup(LED_GREEN_PORT, GPIO_MODE_WIRED_AND, LED_GREEN_PIN);

    setup_capsense();
}

static char output_buffer[65];
static uint32_t output_buffer_pos;

static void usb_putc(void *ign, char c) {
    (void)ign;
    output_buffer[output_buffer_pos++] = c;
    if ((c == '\n') || (output_buffer_pos >= sizeof(output_buffer)-1)) {
        if ((c == '\n') || (output_buffer_pos < sizeof(output_buffer)-1))
            output_buffer[output_buffer_pos++] = '\r';
        output_buffer[output_buffer_pos] = '\0';
        usb_puts(output_buffer);
        output_buffer_pos = 0;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    #define AVERAGE_SIZE 16
    const char *go_to_home = "\x1b[H";
    const char *clear = "\x1b[J";
    int i;
    extern volatile bool g_usbd_is_connected;
    bool usb_was_connected = false;

    uint32_t averages[4][16] = {};
    uint32_t average_position = 0;
    uint32_t last_generation = 0;

    /* Disable the watchdog that the bootloader started. */
    WDOG_CTRL = 0;

    usb_setup();
    init_printf(NULL, usb_putc);
    setup();

    capsense_start();

    while (1) {

        // If USB isn't connected, don't do anything.
        if (!g_usbd_is_connected) {
            usb_was_connected = false;
            continue;
        }

        // If usb wasn't connected (but is now), clear the screen.
        if (!usb_was_connected) {
            udelay_busy(100000);
            usb_puts(clear);
            usb_puts(go_to_home);
            usb_puts("\x1b[3;J\x1b[H\x1b[2J");
            usb_was_connected = true;
        }

        printf("\r\nMeasuring...\r\n");

        while (g_capsense_generation == last_generation)
            ;
        last_generation = g_capsense_generation;

        printf("Measurement generation %6d\n", g_capsense_generation);

        for (i = 0; i < 4; i++) {
            printf("Channel %d:     0x%08x  Max: 0x%08x\n", i, g_channel_values[i], channelMaxValues[i]);
            averages[i][average_position&31] = g_channel_values[i];
            uint32_t average = 0;
            int j;
            for (j = 0; j < AVERAGE_SIZE; j++)
                average += averages[i][j];
            average /= AVERAGE_SIZE;
            printf("        Average: %5d\n", average);
        }
        average_position++;
        if (average_position >= AVERAGE_SIZE)
            average_position = 0;
        usb_puts(go_to_home);

        /*
        printf("TIMER0_CTRL:   0x%08x\n", TIMER0_CTRL);
        printf("TIMER0_STATUS: 0x%08x\n", TIMER0_STATUS);
        printf("TIMER0_CNT:    0x%08x\n", TIMER0_CNT);
        printf("TIMER0_IEN:    0x%08x\n", TIMER0_IEN);
        printf("TIMER0_IF:     0x%08x\n", TIMER0_IF);
        udelay_busy(200000);
        */
    }
}
