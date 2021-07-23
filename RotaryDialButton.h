#pragma once
#include <queue>
class CRotaryDialButton {
public:
	enum Button { BTN_NONE, BTN_LEFT, BTN_RIGHT, BTN_CLICK, BTN_LONGPRESS, BTN0_CLICK, BTN1_CLICK, BTN0_LONGPRESS, BTN1_LONGPRESS, BTN2_LONGPRESS };
    struct ROTARY_DIAL_SETTINGS {
        int m_nLongPressTimerValue;
        int m_nDialSensitivity;
        int m_nDialSpeed;
        bool m_bReverseDial;
    };
    typedef ROTARY_DIAL_SETTINGS ROTARY_DIAL_SETTINGS;
private:
    static ROTARY_DIAL_SETTINGS* pSettings;
    static std::queue<Button> btnBuf;
    static volatile int m_nLongPressTimer;
    static esp_timer_handle_t periodic_LONGPRESS_timer;
    static esp_timer_create_args_t periodic_LONGPRESS_timer_args;
    static int gpioA, gpioB, gpioC, gpioBtn0, gpioBtn1;
#define GPIO_NUMS_COUNT 3
    static int gpioNums[GPIO_NUMS_COUNT]; // only the clicks, not the rotation AB ones
    // int for which one caused the interrupt
    static int whichButton;
    // click array for buttons
	static Button clickBtnArray[3];
	// long press array for buttons
    static Button longpressBtnArray[3];
    // Private constructor so that no objects can be created.
    CRotaryDialButton() {
    }

    // the timer callback for handling long presses
    static void periodic_LONGPRESS_timer_callback(void* arg)
    {
        if (whichButton == -1)
            return;
        noInterrupts();
        Button btn;
		bool level = digitalRead(gpioNums[whichButton]);
        --m_nLongPressTimer;
        // if the timer counter has finished, it must be a long press
		if (m_nLongPressTimer == 0) {
            btn = longpressBtnArray[whichButton];
            // check for both btn's on PCB down
			if ((whichButton == 1 && !digitalRead(gpioNums[2])) || (whichButton == 2 && !digitalRead(gpioNums[1])))
                btn = BTN2_LONGPRESS;
			btnBuf.push(btn);
			// set it so we ignore the button interrupt for one more timer time
			m_nLongPressTimer = -1;
		}
        // if the button is up and the timer hasn't finished counting, it must be a short press
        else if (level) {
			if (m_nLongPressTimer > 0 && m_nLongPressTimer < pSettings->m_nLongPressTimerValue - 1) {
				btn = clickBtnArray[whichButton];
				btnBuf.push(btn);
				m_nLongPressTimer = -1;
			}
            else if (m_nLongPressTimer < -1) {
                esp_timer_stop(periodic_LONGPRESS_timer);
                m_nLongPressTimer = 0;
            }
        }
        interrupts();
    }
    // button interrupt
    static void clickHandler() {
        noInterrupts();
        // figure out who this was
		if (m_nLongPressTimer == 0)
			whichButton = -1;   // indicate this wasn't our interrupt
		for (int ix = 0; ix < GPIO_NUMS_COUNT; ++ix) {
            if (digitalRead(gpioNums[ix]) == 0) {
                whichButton = ix;
                break;
            }
        }
        // our interrupt went low, if timer not started, start it
		if (whichButton != -1 && m_nLongPressTimer == 0) {
            m_nLongPressTimer = pSettings->m_nLongPressTimerValue;
            esp_timer_stop(periodic_LONGPRESS_timer);	// just in case
            esp_timer_start_periodic(periodic_LONGPRESS_timer, 10 * 1000);
        }
        interrupts();
    }
    // interrupt routines for the A and B rotary switches
    // the pendingBtn is used to hold the right rotation until A goes up, this makes
    // the left and right rotation happen at the same apparent time when rotating the dial,
    // unlike without that where the right rotation was faster, I.E. it moved before the
    // click of the button happened.
    static void rotateHandler() {
        noInterrupts();
        // count of buttons for when the sensitivity is reduced from nButtonSensitivity
        static unsigned int countRight = 0;
        static unsigned int countLeft = 0;
        static Button pendingBtn = BTN_NONE;
        static unsigned long lastTime = 0;
        static bool lastValA = true;
        bool valA = digitalRead(gpioA);
        bool valB = digitalRead(gpioB);
        Button btnToPush = BTN_NONE;
        // ignore until the time has expired
        unsigned long millisNow = millis();
        if (millisNow - lastTime > pSettings->m_nDialSpeed) {
            // been too long, reset the counts
            countRight = countLeft = 0;
            // and the pending one
            pendingBtn = BTN_NONE;
        }
        if (lastValA != valA && millisNow > lastTime + 3) {
            if (pendingBtn != BTN_NONE) {
                btnToPush = pendingBtn;
                pendingBtn = BTN_NONE;
            }
            else if (lastValA && !valA) {
                Button btn = (pSettings->m_bReverseDial ? !valB : valB) ? BTN_RIGHT : BTN_LEFT;
                if (btn == BTN_RIGHT) {
                    pendingBtn = btn;
                }
                else {
                    btnToPush = btn;
                }
            }
            lastValA = valA;
            // push a button?
            if (btnToPush != BTN_NONE) {
                // check sensitivity counts
                if (btnToPush == BTN_RIGHT)
                    ++countRight;
                else
                    ++countLeft;
                if (countRight >= pSettings->m_nDialSensitivity || countLeft >= pSettings->m_nDialSensitivity) {
                    btnBuf.push(btnToPush);
                    countRight = countLeft = 0;
                }
            }
            // remember when we were here last time
            lastTime = millisNow;
        }
        interrupts();
    }

    // public things
public:
	static void begin(int a, int b, int c, int btn0, int btn1, ROTARY_DIAL_SETTINGS* ps) {
        // first time, set things up
        pSettings = ps;
        pSettings->m_nLongPressTimerValue = 40;
        pSettings->m_nDialSensitivity = 1;
        pSettings->m_nDialSpeed = 300;
        pSettings->m_bReverseDial = false;
        gpioA = a;
        gpioB = b;
        gpioC = c;
        gpioBtn0 = btn0;
        gpioBtn1 = btn1;
        gpioNums[0] = c;
        gpioNums[1] = btn0;
        gpioNums[2] = btn1;
        // create a timer
        // set the interrupts
        periodic_LONGPRESS_timer_args = {
                periodic_LONGPRESS_timer_callback,
                /* argument specified here will be passed to timer callback function */
                (void*)0,
                ESP_TIMER_TASK,
                "one-shotLONGPRESS"
        };
        esp_timer_create(&periodic_LONGPRESS_timer_args, &periodic_LONGPRESS_timer);
        // pinMode() doesn't work on Heltec for pin14, strange
        // load the buttons, A and B are the dial, and C is the click
        // btn0/1 are the two buttons on the TTGO, use -1 to ignore
        gpio_set_direction((gpio_num_t)a, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)a, GPIO_PULLUP_ONLY);
        gpio_set_direction((gpio_num_t)b, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)b, GPIO_PULLUP_ONLY);
        gpio_set_direction((gpio_num_t)c, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)c, GPIO_PULLUP_ONLY);
        if (gpioBtn0 != -1) {
            gpio_set_direction((gpio_num_t)gpioBtn0, GPIO_MODE_INPUT);
            gpio_set_pull_mode((gpio_num_t)gpioBtn0, GPIO_PULLUP_ONLY);
            attachInterrupt(gpioBtn0, clickHandler, FALLING);
        }
        if (gpioBtn1 != -1) {
            gpio_set_direction((gpio_num_t)gpioBtn1, GPIO_MODE_INPUT);
            gpio_set_pull_mode((gpio_num_t)gpioBtn1, GPIO_PULLUP_ONLY);
            attachInterrupt(gpioBtn1, clickHandler, FALLING);
        }
        attachInterrupt(gpioC, clickHandler, FALLING);
        attachInterrupt(gpioA, rotateHandler, CHANGE);
    }
    // see what the next button is, return None if queue empty
    static Button peek() {
        if (btnBuf.empty())
            return BTN_NONE;
        return btnBuf.front();
    }
    // get the next button and remove from the queue, return BTN_NONE if nothing there
    static Button dequeue() {
        Button btn = BTN_NONE;
        if (!btnBuf.empty()) {
            btn = btnBuf.front();
            btnBuf.pop();
        }
        return btn;
    }
    // wait milliseconds for a button, optionally return click or none
    // waitTime -1 means forever
    static Button waitButton(bool bClick, int waitTime = -1) {
        Button ret = bClick ? BTN_CLICK : BTN_NONE;
        unsigned long nowTime = millis();
        while (waitTime == -1 || millis() < nowTime + waitTime) {
            if (!btnBuf.empty()) {
                ret = dequeue();
                break;
            }
            delay(10);
        }
        return ret;
    }
    // clear the buffer
    static void clear() {
        while (btnBuf.size())
            btnBuf.pop();
    }
    // return the count
    static int getCount() {
        return btnBuf.size();
    }
    // push a button
    static void pushButton(Button btn) {
        btnBuf.push(btn);
    }
};
std::queue<enum CRotaryDialButton::Button> CRotaryDialButton::btnBuf;
volatile int CRotaryDialButton::m_nLongPressTimer;
esp_timer_handle_t CRotaryDialButton::periodic_LONGPRESS_timer;
esp_timer_create_args_t CRotaryDialButton::periodic_LONGPRESS_timer_args;
int CRotaryDialButton::gpioA, CRotaryDialButton::gpioB, CRotaryDialButton::gpioC;
int CRotaryDialButton::gpioBtn0, CRotaryDialButton::gpioBtn1;
CRotaryDialButton::ROTARY_DIAL_SETTINGS* CRotaryDialButton::pSettings = { NULL };
int CRotaryDialButton::gpioNums[GPIO_NUMS_COUNT] = { 0 };
int CRotaryDialButton::whichButton;
CRotaryDialButton::Button CRotaryDialButton::longpressBtnArray[3] = { CRotaryDialButton::BTN_LONGPRESS,CRotaryDialButton::BTN0_LONGPRESS,CRotaryDialButton::BTN1_LONGPRESS };
CRotaryDialButton::Button CRotaryDialButton::clickBtnArray[3] = { CRotaryDialButton::BTN_CLICK,CRotaryDialButton::BTN0_CLICK,CRotaryDialButton::BTN1_CLICK };
