#include <stdint.h>
#include <MK64F12.h>

#define MASK(x)   ((uint32_t)1u << (x))


#define SW1   3u
#define SW2   10u
#define SW3   11u
#define BUZ   1u
#define LATCH 23u
#define CLK   3u
#define DATA  12u
static const uint8_t LED_PIN[4] = { 1u, 3u, 2u, 0u };


typedef enum { ST_SET, ST_RUN, ST_PAUSE, ST_ALARM } state_t;

static volatile state_t  g_state = ST_SET;
static volatile uint16_t g_seconds = 0;
static volatile uint32_t g_ms = 0;
static volatile uint8_t  g_sec_flag = 0;
static volatile uint16_t g_sec_count = 0;
static volatile uint8_t  g_btn = 0;
static uint32_t g_bus_hz = 0;


void led_init(void)
{
    uint8_t i;
    SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
    for (i = 0; i < 4u; i++) {
        PORTD->PCR[LED_PIN[i]] = PORT_PCR_MUX(1);
        PTD->PSOR  = MASK(LED_PIN[i]);
        PTD->PDDR |= MASK(LED_PIN[i]);
    }
}

void led_write(uint8_t pattern)
{
    uint8_t i;
    for (i = 0; i < 4u; i++) {
        if (pattern & (1u << i)) PTD->PCOR = MASK(LED_PIN[i]);
        else                     PTD->PSOR = MASK(LED_PIN[i]);
    }
}


void switch_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;

    PORTB->PCR[SW1] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);
    PORTB->PCR[SW2] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);
    PORTB->PCR[SW3] = PORT_PCR_MUX(1) | PORT_PCR_PE_MASK | PORT_PCR_PS_MASK | PORT_PCR_IRQC(0xA);
    PTB->PDDR &= ~(MASK(SW1) | MASK(SW2) | MASK(SW3));
    PORTB->ISFR = 0xFFFFFFFFu;
    NVIC_EnableIRQ(PORTB_IRQn);
}

void PORTB_IRQHandler(void)
{
    static uint32_t last_ms = 0;
    uint32_t flags = PORTB->ISFR;

    PORTB->ISFR = flags;
    if ((g_ms - last_ms) < 150u) {
        return;
    }
    last_ms = g_ms;
    if (flags & MASK(SW1)) g_btn |= 0x01u;
    if (flags & MASK(SW2)) g_btn |= 0x02u;
    if (flags & MASK(SW3)) g_btn |= 0x04u;
}


static const uint8_t SEG_NUM[10] = { 0xC0, 0xF9, 0xA4, 0xB0, 0x99, 0x92, 0x82, 0xF8, 0x80, 0x90 };
static const uint8_t SEG_DIGIT[4] = { 0xF1, 0xF2, 0xF4, 0xF8 };
#define SEG_E     0x86u
#define SEG_n     0xABu
#define SEG_d     0xA1u
#define SEG_OFF   0xFFu
static volatile uint8_t g_disp[4] = { 0xC0, 0xC0, 0xC0, 0xC0 };

void seg7_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK | SIM_SCGC5_PORTC_MASK;
    PORTB->PCR[LATCH] = PORT_PCR_MUX(1);
    PORTC->PCR[CLK]   = PORT_PCR_MUX(1);
    PORTC->PCR[DATA]  = PORT_PCR_MUX(1);
    PTB->PDDR |= MASK(LATCH);
    PTC->PDDR |= MASK(CLK) | MASK(DATA);
}

void seg7_shift(uint8_t value)
{
    uint8_t i;
    for (i = 0; i < 8u; i++) {
        if (value & 0x80u) PTC->PSOR = MASK(DATA);
        else               PTC->PCOR = MASK(DATA);
        PTC->PSOR = MASK(CLK);
        PTC->PCOR = MASK(CLK);
        value <<= 1;
    }
}

void seg7_refresh(void)
{
    static uint8_t pos = 0;
    PTB->PCOR = MASK(LATCH);
    seg7_shift(g_disp[pos]);
    seg7_shift(SEG_DIGIT[pos]);
    PTB->PSOR = MASK(LATCH);
    pos = (uint8_t)((pos + 1u) % 4u);
}

void seg7_show_time(uint16_t sec, uint8_t dot)
{
    uint8_t mm = (uint8_t)(sec / 60u), ss = (uint8_t)(sec % 60u);
    g_disp[0] = SEG_NUM[mm / 10u];
    g_disp[1] = dot ? (uint8_t)(SEG_NUM[mm % 10u] & 0x7Fu) : SEG_NUM[mm % 10u];
    g_disp[2] = SEG_NUM[ss / 10u];
    g_disp[3] = SEG_NUM[ss % 10u];
}

void seg7_show_end(uint8_t on)
{
    g_disp[0] = on ? SEG_E : SEG_OFF;
    g_disp[1] = on ? SEG_n : SEG_OFF;
    g_disp[2] = on ? SEG_d : SEG_OFF;
    g_disp[3] = SEG_OFF;
}


typedef struct { uint16_t freq; uint16_t ms; } note_t;

static const note_t SONG[] = {
    {523,300},{523,300},{784,300},{784,300},{880,300},{880,300},{784,600},
    {698,300},{698,300},{659,300},{659,300},{587,300},{587,300},{523,600},{0,500}
};
#define SONG_LEN  (sizeof(SONG) / sizeof(SONG[0]))

static volatile uint16_t g_beep_ms = 0;
static volatile uint8_t  g_playing = 0;
static volatile uint8_t  g_note = 0;
static volatile uint16_t g_note_ms = 0;

void buzzer_on(void)
{
    PTA->PCOR  = MASK(BUZ);
    PTA->PDDR |= MASK(BUZ);
}

void buzzer_off(void)
{
    PTA->PSOR  = MASK(BUZ);
    PTA->PDDR &= ~MASK(BUZ);
}

void buzzer_init(void)
{
    SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;
    PORTA->PCR[BUZ] = PORT_PCR_MUX(1);
    buzzer_off();
}

void buzzer_beep(uint16_t ms)
{
    g_beep_ms = ms;
    buzzer_on();
}

void tone_start(uint16_t freq)
{
    PIT->CHANNEL[1].TCTRL = 0;
    if (freq == 0u) { buzzer_off(); return; }
    PTA->PDDR |= MASK(BUZ);
    PIT->CHANNEL[1].LDVAL = g_bus_hz / (2u * freq) - 1u;
    PIT->CHANNEL[1].TCTRL = PIT_TCTRL_TIE_MASK | PIT_TCTRL_TEN_MASK;
}

void tone_stop(void)
{
    PIT->CHANNEL[1].TCTRL = 0;
    PIT->CHANNEL[1].TFLG  = PIT_TFLG_TIF_MASK;
    NVIC_ClearPendingIRQ(PIT1_IRQn);
    buzzer_off();
}

void song_play(void)
{
    g_note = 0;
    g_note_ms = SONG[0].ms;
    g_playing = 1;
    tone_start(SONG[0].freq);
}

void song_stop(void)
{
    g_playing = 0;
    tone_stop();
}

void buzzer_update_1ms(void)
{
    if (g_beep_ms > 0u && --g_beep_ms == 0u) {
        buzzer_off();
    }
    if (!g_playing) {
        return;
    }
    if (g_note_ms == 50u) {
        tone_stop();
    }
    if (--g_note_ms == 0u) {
        g_note = (uint8_t)((g_note + 1u) % SONG_LEN);
        g_note_ms = SONG[g_note].ms;
        tone_start(SONG[g_note].freq);
    }
}


void timer_init(void)
{
    uint32_t div1, div2;


    SystemCoreClockUpdate();
    div1 = ((SIM->CLKDIV1 >> 28) & 0xFu) + 1u;
    div2 = ((SIM->CLKDIV1 >> 24) & 0xFu) + 1u;
    g_bus_hz = SystemCoreClock / div2 * div1;

    SIM->SCGC6 |= SIM_SCGC6_PIT_MASK;
    PIT->MCR = 0;
    PIT->CHANNEL[0].LDVAL = g_bus_hz / 1000u - 1u;
    PIT->CHANNEL[0].TCTRL = PIT_TCTRL_TIE_MASK | PIT_TCTRL_TEN_MASK;

    NVIC_SetPriority(PIT1_IRQn, 1);
    NVIC_SetPriority(PIT0_IRQn, 2);
    NVIC_SetPriority(PORTB_IRQn, 3);
    NVIC_EnableIRQ(PIT0_IRQn);
    NVIC_EnableIRQ(PIT1_IRQn);
}

void PIT0_IRQHandler(void)
{
    PIT->CHANNEL[0].TFLG = PIT_TFLG_TIF_MASK;
    g_ms++;
    seg7_refresh();
    buzzer_update_1ms();

    if (g_state == ST_RUN && ++g_sec_count >= 1000u) {
        g_sec_count = 0;
        if (g_seconds > 0u) g_seconds--;
        g_sec_flag = 1;
    }
}

void PIT1_IRQHandler(void)
{
    PIT->CHANNEL[1].TFLG = PIT_TFLG_TIF_MASK;
    if (PIT->CHANNEL[1].TCTRL & PIT_TCTRL_TEN_MASK) {
        PTA->PTOR = MASK(BUZ);
    }
}


uint8_t read_buttons(void)
{
    uint8_t b;
    __disable_irq();
    b = g_btn;
    g_btn = 0;
    __enable_irq();
    return b;
}

void handle_buttons(void)
{
    uint8_t b = read_buttons();
    if (b == 0u) return;

    if (g_state == ST_ALARM) {
        song_stop();
        g_seconds = 0;
        g_state = ST_SET;
        return;
    }
    buzzer_beep(30);

    if ((b & 0x01u) && g_state != ST_RUN) g_seconds = (uint16_t)((g_seconds + 60u) % 6000u);
    if ((b & 0x02u) && g_state != ST_RUN) g_seconds = (uint16_t)((g_seconds + 10u) % 6000u);
    if (b & 0x04u) {
        if (g_state == ST_RUN)                       g_state = ST_PAUSE;
        else if (g_seconds > 0u) { g_sec_count = 0;  g_state = ST_RUN; }
    }
}

void handle_every_second(void)
{
    if (!g_sec_flag) return;
    g_sec_flag = 0;

    if (g_seconds == 0u) {
        g_state = ST_ALARM;
        song_play();
    } else if (g_seconds <= 5u) {
        buzzer_beep(100);
    }
}

void update_output(void)
{
    uint8_t blink = (uint8_t)((g_ms / 500u) % 2u);

    switch (g_state) {
    case ST_SET:   seg7_show_time(g_seconds, 1);     led_write(0x0);              break;
    case ST_RUN:   seg7_show_time(g_seconds, blink); led_write(0x1);              break;
    case ST_PAUSE: seg7_show_time(g_seconds, 1);     led_write(blink ? 0x2 : 0);  break;
    case ST_ALARM: seg7_show_end(blink);             led_write(blink ? 0xF : 0);  break;
    }
}


int main(void)
{
    led_init();
    buzzer_init();
    seg7_init();
    timer_init();
    switch_init();

    while (1) {
        handle_buttons();
        handle_every_second();
        update_output();
    }
}
