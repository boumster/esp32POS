#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h" // Ensure IIC_SDA, IIC_SCL, TP_INT are defined here
#include <Wire.h>
#include "HWCDC.h"
#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_IOExpander_Library.h>   // Added for IO Expander
#include "Arduino_DriveBus_Library.h" // Added for IIC DriveBus
#include <Preferences.h>

#define MY_HOR_RES 368
#define MY_VER_RES 448

const char *ntpServer = "120.25.108.11";
int net_flag = 0;

HWCDC USBSerial;
Preferences preferences;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_GFX *gfx = new Arduino_SH8601(bus, -1, 0, false, MY_HOR_RES, MY_VER_RES);

// LVGL display buffer and driver
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[MY_HOR_RES * 40];

// Touch controller and I2C bus
#define _EXAMPLE_CHIP_CLASS(name, ...) ESP_IOExpander_##name(__VA_ARGS__)
#define EXAMPLE_CHIP_CLASS(name, ...) _EXAMPLE_CHIP_CLASS(name, ##__VA_ARGS__)

ESP_IOExpander *expander = NULL; // IO Expander (if used by your board for touch/reset)

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus;
std::unique_ptr<Arduino_IIC> touch_controller; // Using a generic name, assuming FT3x68 or similar

// Forward declaration for interrupt handler
void Arduino_IIC_Touch_Interrupt(void);

// Forward declaration for main ui
void create_main_app_ui();

// LVGL Tick period
#define EXAMPLE_LVGL_TICK_PERIOD_MS 2

static void my_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
  lv_disp_flush_ready(disp);
}

// Touch interrupt handler function
void Arduino_IIC_Touch_Interrupt(void)
{
  if (touch_controller)
  {
    touch_controller->IIC_Interrupt_Flag = true;
  }
}

// LVGL tick custom handler
static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

// Read touchpad function
static void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  if (touch_controller)
  {
    int32_t touchX = touch_controller->IIC_Read_Device_Value(touch_controller->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t touchY = touch_controller->IIC_Read_Device_Value(touch_controller->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    if (touch_controller->IIC_Interrupt_Flag == true)
    {
      touch_controller->IIC_Interrupt_Flag = false;
      data->state = LV_INDEV_STATE_PR; // Pressed
      data->point.x = touchX;
      data->point.y = touchY;
    }
    else
    {
      data->state = LV_INDEV_STATE_REL; // Released
    }
  }
  else
  {
    data->state = LV_INDEV_STATE_REL; // Released if no touch controller
  }
}

// LVG UI Objects
lv_obj_t *qr = nullptr; // Global pointer to the QR code object
lv_obj_t *qr_page_status_label = nullptr;
lv_obj_t *tabview;
lv_obj_t *page_qr; // The page/tab where QR code and button will reside

// Login related objects
lv_obj_t *page_info;
lv_obj_t *ta_access_key;      // Text area for access key input
lv_obj_t *kb;                 // Keyboard object for text input
lv_obj_t *login_status_label; // Label to show login status on info page

// Invoice related objects
lv_obj_t *btn_generate_invoice;          // Button to generate invoice on QR page
lv_obj_t *label_generate_invoice;        // Label for the invoice button
lv_obj_t *ta_invoice_amount = nullptr;   // Text area for invoice amount
lv_obj_t *dd_invoice_currency = nullptr; // Dropdown for invoice currency

// eCash related objects
lv_obj_t *page_ecash = nullptr;
lv_obj_t *ta_ecash_amount = nullptr;
lv_obj_t *btn_generate_ecash = nullptr;
lv_obj_t *ecash_qr_widget = nullptr; // For displaying the eCash QR code
lv_obj_t *ecash_page_status_label = nullptr;

// Pay Address page objects
lv_obj_t *page_pay_address = nullptr;
lv_obj_t *ta_pay_address_address = nullptr;
lv_obj_t *ta_pay_address_amount = nullptr;
lv_obj_t *dd_pay_address_currency = nullptr;
lv_obj_t *ta_pay_address_comment = nullptr;
lv_obj_t *btn_pay_address = nullptr;
lv_obj_t *pay_address_status_label = nullptr;

// LVGL objects for WiFi login page
static lv_obj_t *wifi_login_screen = nullptr;
static lv_obj_t *wifi_ssid_dropdown = nullptr;
static lv_obj_t *wifi_password_ta = nullptr;
static lv_obj_t *wifi_connect_btn = nullptr;
static lv_obj_t *wifi_status_label = nullptr;

lv_obj_t *btn_refresh_info = nullptr;

void show_qr_code(const char *data)
{
  USBSerial.print("show_qr_code called with: ");
  USBSerial.println(data);

  if (!page_qr)
  {
    USBSerial.println("QR page is not initialized! Cannot show QR or status.");
    return;
  }

  // Clear previous QR code if it exists
  if (qr != nullptr)
  {
    lv_obj_del(qr);
    qr = nullptr;
  }
  // Clear previous status label if it exists
  if (qr_page_status_label != nullptr)
  {
    lv_obj_del(qr_page_status_label);
    qr_page_status_label = nullptr;
  }

  // Determine if the data is a status message or actual QR data
  bool is_status_msg = false;
  String dataStr = String(data);

  if (dataStr.equals("Log in via 'Login Key' tab.") ||
      dataStr.equals("Login Failed. Please try again.") ||
      dataStr.equals("Login Required") ||
      dataStr.equals("Logged In. Ready for invoice.") ||
      dataStr.startsWith("API Error:") ||
      dataStr.startsWith("Parse Err:") ||
      dataStr.equals("HTTP Conn Error") ||
      dataStr.equals("WiFi Error"))
  {
    is_status_msg = true;
  }

  if (is_status_msg)
  {
    USBSerial.println("Displaying as status message.");
    qr_page_status_label = lv_label_create(page_qr);
    lv_label_set_text(qr_page_status_label, data);
    lv_label_set_long_mode(qr_page_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(qr_page_status_label, lv_pct(90)); // Use 90% of page width for wrapping
    lv_obj_set_style_text_align(qr_page_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(qr_page_status_label);
    lv_obj_clear_flag(qr_page_status_label, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    USBSerial.println("Displaying as QR code.");
    // Create new QR code
    qr = lv_qrcode_create(page_qr, 300, lv_color_hex3(0x33f), lv_color_hex3(0xeef));
    if (qr != nullptr)
    {
      lv_qrcode_update(qr, data, strlen(data));
      lv_obj_align(qr, LV_ALIGN_CENTER, 0, 40);
      lv_obj_clear_flag(qr, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
      USBSerial.println("Failed to create QR code object!");
      // Fallback to show error as text if QR creation fails
      qr_page_status_label = lv_label_create(page_qr);
      lv_label_set_text(qr_page_status_label, "Error: QR Gen Failed");
      lv_obj_center(qr_page_status_label);
    }
  }
}

// Helper function to create the login form on page_info
void create_login_form_on_info_page()
{
  if (!page_info)
    return;
  lv_obj_clean(page_info); // Clear it first in case of any prior state

  lv_obj_t *label_info_title = lv_label_create(page_info);
  lv_label_set_text(label_info_title, "Manual Login Required");
  lv_obj_align(label_info_title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *label_access_key_prompt = lv_label_create(page_info);
  lv_label_set_text(label_access_key_prompt, "Enter Access Key:");
  lv_obj_align_to(label_access_key_prompt, label_info_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);

  ta_access_key = lv_textarea_create(page_info); // Assign to global
  lv_obj_set_width(ta_access_key, lv_pct(90));
  lv_textarea_set_one_line(ta_access_key, true);
  lv_textarea_set_placeholder_text(ta_access_key, "e.g., 123-abc-xyz");
  lv_obj_align_to(ta_access_key, label_access_key_prompt, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  lv_obj_add_event_cb(ta_access_key, ta_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t *btn_login_local = lv_btn_create(page_info); // Local var for the button itself
  lv_obj_set_width(btn_login_local, lv_pct(50));
  lv_obj_align_to(btn_login_local, ta_access_key, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
  lv_obj_add_event_cb(btn_login_local, btn_login_info_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *label_btn_login_info = lv_label_create(btn_login_local);
  lv_label_set_text(label_btn_login_info, "Login with this Key");
  lv_obj_center(label_btn_login_info);

  login_status_label = lv_label_create(page_info); // Assign to global
  lv_label_set_text(login_status_label, "Enter key and press login.");
  lv_obj_set_width(login_status_label, lv_pct(90));
  lv_label_set_long_mode(login_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(login_status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(login_status_label, btn_login_local, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);
}

// Event handler for the refresh button on the info page
static void btn_refresh_info_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CLICKED)
  {
    USBSerial.println("Refresh button clicked, rebuilding main UI.");
    create_main_app_ui();
  }
}

void perform_safebox_access_check(const String &access_token_cookie)
{
  if (!page_info)
  {
    USBSerial.println("[Access Check] page_info is null. Cannot update UI.");
    return;
  }
  lv_obj_clean(page_info); // Clear all children from page_info

  if (access_token_cookie.length() == 0)
  {
    USBSerial.println("[Access Check] No access token provided.");
    lv_obj_t *err_label = lv_label_create(page_info);
    lv_label_set_text(err_label, "Error: Not Logged In.\nPlease use 'Login Key' tab."); // This message might be confusing if already on info page
    lv_obj_center(err_label);
    // Also update QR page
    show_qr_code("Login Required");
    if (btn_generate_invoice)
      lv_obj_add_flag(btn_generate_invoice, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    USBSerial.println("[Access Check] WiFi not connected.");
    lv_obj_t *err_label = lv_label_create(page_info);
    lv_label_set_text(err_label, "WiFi Connection Error.\nCannot fetch account details.");
    lv_obj_center(err_label);
    show_qr_code("WiFi Error");
    return;
  }

  HTTPClient http_access;
  String accessApiUrl = "https://getsafebox.app/safebox/access";
  USBSerial.println("\n[Access Check] Calling for Account Info Page: " + accessApiUrl);

  http_access.begin(accessApiUrl);
  http_access.addHeader("accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7");
  http_access.addHeader("Cookie", access_token_cookie.c_str());
  http_access.setUserAgent("ESP32HTTPClient/1.0 (Compatible; MyApp/1.0)");

  int httpCodeAccess = http_access.GET();
  String fiatBalance = "N/A";
  String btcBalance = "N/A";
  String qrData = "Error: QR Data Not Found"; // Default for QR data

  if (httpCodeAccess > 0)
  {
    USBSerial.printf("[Access Check] HTTP GET response code: %d\n", httpCodeAccess);
    String payloadAccess = http_access.getString();
    USBSerial.println("[Access Check] Response payload for GET /safebox/access:"); // Full payload can be long
    USBSerial.println(payloadAccess);

    String fiatStartTag = "id=\"fiat_balance\"";
    int fiatStartIndex = payloadAccess.indexOf(fiatStartTag);
    if (fiatStartIndex != -1)
    {
      fiatStartIndex = payloadAccess.indexOf('>', fiatStartIndex + fiatStartTag.length());
      if (fiatStartIndex != -1)
      {
        int fiatEndIndex = payloadAccess.indexOf("<", fiatStartIndex + 1);
        if (fiatEndIndex != -1) {
          fiatBalance = payloadAccess.substring(fiatStartIndex + 1, fiatEndIndex);
          fiatBalance.trim();
        }
      }
    }
    USBSerial.println("[Access Check] Extracted Fiat Balance: " + fiatBalance);

    String btcStartTag = "id=\"heading_balance\"";
    int btcStartIndex = payloadAccess.indexOf(btcStartTag);
    if (btcStartIndex != -1)
    {
      btcStartIndex = payloadAccess.indexOf('>', btcStartIndex + btcStartTag.length());
      if (btcStartIndex != -1)
      {
        int btcEndIndex = payloadAccess.indexOf("<", btcStartIndex + 1);
        if (btcEndIndex != -1) {
          btcBalance = payloadAccess.substring(btcStartIndex + 1, btcEndIndex);
          btcBalance.trim();
        }
      }
    }
    USBSerial.println("[Access Check] Extracted Bitcoin Balance: " + btcBalance);

    String qrImgTagStart = "<img id=\"img_qr_display\"";
    int qrImgStartIndex = payloadAccess.indexOf(qrImgTagStart);
    if (qrImgStartIndex != -1)
    {
      String srcAttrStart = "src=\"/safebox/qr/";
      int srcStartIndex = payloadAccess.indexOf(srcAttrStart, qrImgStartIndex);
      if (srcStartIndex != -1)
      {
        srcStartIndex += srcAttrStart.length();
        int srcEndIndex = payloadAccess.indexOf("\"", srcStartIndex);
        if (srcEndIndex != -1)
          qrData = payloadAccess.substring(srcStartIndex, srcEndIndex);
      }
    }
    USBSerial.println("[Access Check] Extracted QR Data for Info Page: " + qrData);

    // --- Display parsed data on page_info ---
    lv_obj_t *title_label = lv_label_create(page_info);
    lv_label_set_text(title_label, "Account Overview");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *fiat_val_label = lv_label_create(page_info);
    lv_label_set_text_fmt(fiat_val_label, "Fiat: %s", fiatBalance.c_str());
    lv_obj_align_to(fiat_val_label, title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 5, 10);

    lv_obj_t *btc_val_label = lv_label_create(page_info);
    lv_label_set_text_fmt(btc_val_label, "Sats: %s", btcBalance.c_str());
    lv_obj_align_to(btc_val_label, fiat_val_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);

    // Display QR data (lightning address) as text or QR on page_info
    bool displayQrAsText = qrData.startsWith("Error:") || qrData.length() > 50; // Example condition for text

    if (displayQrAsText)
    {
      lv_obj_t *qr_text_label = lv_label_create(page_info);
      lv_label_set_text_fmt(qr_text_label, "LN Address:\n%s", qrData.c_str());
      lv_label_set_long_mode(qr_text_label, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(qr_text_label, lv_pct(90));
      lv_obj_align_to(qr_text_label, btc_val_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    }
    else
    {
      lv_obj_t *qr_display_info_page = lv_qrcode_create(page_info, 240, lv_color_black(), lv_color_white()); // Smaller QR
      if (qr_display_info_page)
      {
        lv_qrcode_update(qr_display_info_page, qrData.c_str(), qrData.length());
        lv_obj_align(qr_display_info_page, LV_ALIGN_CENTER, 0, 40);
      }
      else
      {
        lv_obj_t *qr_err_label = lv_label_create(page_info);
        lv_label_set_text(qr_err_label, "Error: QR Gen Failed");
        lv_obj_align_to(qr_err_label, btc_val_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
      }
    }
    // Create refresh button
    btn_refresh_info = lv_btn_create(page_info);
    lv_obj_align(btn_refresh_info, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(btn_refresh_info, btn_refresh_info_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label_refresh_info = lv_label_create(btn_refresh_info);
    lv_label_set_text(label_refresh_info, LV_SYMBOL_REFRESH);
    lv_obj_center(label_refresh_info);

    // Update the main QR page as well to show generic "Logged In"
    show_qr_code("Logged In. Ready for invoice.");
    if (btn_generate_invoice)
      lv_obj_clear_flag(btn_generate_invoice, LV_OBJ_FLAG_HIDDEN);
  }
  else
  {
    USBSerial.printf("[Access Check] HTTP GET request failed, error: %s\n", http_access.errorToString(httpCodeAccess).c_str());
    lv_obj_t *err_label = lv_label_create(page_info);
    lv_label_set_text(err_label, "Failed to fetch account details.\nPlease check connection or try login again.");
    lv_obj_set_style_text_align(err_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(err_label);
    // Also update QR page
    show_qr_code("HTTP Conn Error");
  }
  http_access.end();
}

bool perform_login_and_store_token(const char *access_key_value)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    USBSerial.println("[Login] WiFi not connected. Aborting login.");
    return false;
  }

  HTTPClient http_login;
  Preferences preferences_login;
  String loginApiUrl = "https://getsafebox.app/safebox/login";

  String formData = "access_key=" + String(access_key_value);

  USBSerial.println("[Login] Attempting login to: " + loginApiUrl);
  USBSerial.println("[Login] Using form data: " + formData);

  http_login.begin(loginApiUrl);
  http_login.addHeader("accept", "application/json");
  http_login.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http_login.setUserAgent("ESP32HTTPClient/1.0");

  const char *headerKeys[] = {"Set-Cookie", "Location"};
  size_t numCollectHeaders = sizeof(headerKeys) / sizeof(char *);
  http_login.collectHeaders(headerKeys, numCollectHeaders);

  USBSerial.println("[Login] Sending POST request...");
  int httpCodeLogin = http_login.POST(formData);
  String responsePayloadLogin;
  bool tokenStored = false;
  String accessTokenFullCookie; // Declare here to be accessible for the new function call

  if (httpCodeLogin > 0)
  {
    responsePayloadLogin = http_login.getString();
    USBSerial.printf("[Login] HTTP POST response code: %d\n", httpCodeLogin);
    USBSerial.println("[Login] Response payload for POST /safebox/login:");
    USBSerial.println(responsePayloadLogin);

    if (httpCodeLogin == HTTP_CODE_OK ||
        httpCodeLogin == HTTP_CODE_FOUND ||
        httpCodeLogin == HTTP_CODE_MOVED_PERMANENTLY ||
        httpCodeLogin == HTTP_CODE_SEE_OTHER ||
        httpCodeLogin == HTTP_CODE_TEMPORARY_REDIRECT ||
        httpCodeLogin == HTTP_CODE_PERMANENT_REDIRECT)
    {

      if (httpCodeLogin != HTTP_CODE_OK)
      {
        USBSerial.printf("[Login] Received redirect code %d.\n", httpCodeLogin);
        String location = http_login.header("Location");
        if (location.length() > 0)
        {
          USBSerial.println("[Login] Redirect Location: " + location);
        }
      }

      String collectedSetCookie = http_login.header("Set-Cookie");

      if (collectedSetCookie.length() > 0)
      {
        USBSerial.println("[Login] Collected Set-Cookie: " + collectedSetCookie);
        if (collectedSetCookie.startsWith("access_token="))
        {
          int semicolonIndex = collectedSetCookie.indexOf(';');
          if (semicolonIndex != -1)
          {
            accessTokenFullCookie = collectedSetCookie.substring(0, semicolonIndex);
          }
          else
          {
            accessTokenFullCookie = collectedSetCookie;
          }
          USBSerial.print("[Login] Extracted access_token cookie part: ");
          USBSerial.println(accessTokenFullCookie);

          preferences_login.begin("app_prefs", false);
          preferences_login.putString("accessToken", accessTokenFullCookie);
          preferences_login.end();
          USBSerial.println("[Login] Access token stored in NVS.");
          tokenStored = true;
        }
        else
        {
          USBSerial.println("[Login] 'access_token=' not found at the start of the collected Set-Cookie header.");
        }
      }
      else
      {
        USBSerial.println("[Login] Collected Set-Cookie header was empty or not found by .header(\"Set-Cookie\").");
        int numHeaders = http_login.headers();
        if (numHeaders > 0 && !tokenStored)
        {
          USBSerial.println("[Login] Iterating through all reported headers as fallback:");
          for (int i = 0; i < numHeaders; i++)
          {
            String headerName = http_login.headerName(i);
            String headerValue = http_login.header(i);
            if (headerName.equalsIgnoreCase("Set-Cookie"))
            {
              USBSerial.printf("[Login] Found Set-Cookie in iteration: %s\n", headerValue.c_str());
              if (headerValue.startsWith("access_token="))
              {
                int semicolonIndex = headerValue.indexOf(';');
                if (semicolonIndex != -1)
                {
                  accessTokenFullCookie = headerValue.substring(0, semicolonIndex);
                }
                else
                {
                  accessTokenFullCookie = headerValue;
                }
                USBSerial.print("[Login] Extracted access_token from iteration: ");
                USBSerial.println(accessTokenFullCookie);
                preferences_login.begin("app_prefs", false);
                preferences_login.putString("accessToken", accessTokenFullCookie);
                preferences_login.end();
                USBSerial.println("[Login] Access token stored in NVS from iteration.");
                tokenStored = true;
                break;
              }
            }
          }
        }
      }

      if (!tokenStored)
      {
        USBSerial.println("[Login] Failed to find and store access_token from Set-Cookie headers.");
      }
      else
      {
        // Token was stored, now make the /safebox/access call using the new function
        perform_safebox_access_check(accessTokenFullCookie);
      }
    }
    else
    {
      USBSerial.printf("[Login] Login request returned unhandled HTTP code: %d. Payload was printed above.\n", httpCodeLogin);
    }
  }
  else
  {
    USBSerial.printf("[Login] HTTP POST request failed, error: %s\n", http_login.errorToString(httpCodeLogin).c_str());
  }
  http_login.end();
  return tokenStored;
}

static void btn_event_cb(lv_event_t *e) // This is for the "Generate Invoice" button on page_qr
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CLICKED)
  {
    USBSerial.println("Generate Invoice Button Clicked - Attempting to fetch API data via POST...");
    // This is to ensure the keyboard is hidden after login button click
    if (kb != NULL)
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    if (qr != nullptr)
    {
      lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
    }
    if (qr_page_status_label != nullptr)
    {
      lv_obj_add_flag(qr_page_status_label, LV_OBJ_FLAG_HIDDEN);
    }
    // Consider showing a spinner here if you have one configured globally

    lv_timer_handler();
    delay(50);

    if (WiFi.status() == WL_CONNECTED)
    {
      HTTPClient http_invoice;
      Preferences preferences;

      preferences.begin("app_prefs", true);
      String accessTokenCookie = preferences.getString("accessToken", "");
      preferences.end();

      if (accessTokenCookie.length() == 0)
      {
        USBSerial.println("Access token not found in NVS for invoice generation.");
        show_qr_code("Login Required");
        // Potentially switch to login tab or show more prominent login message
        return;
      }

      // Get amount and currency from the new input fields
      const char *amount_str = "0";
      if (ta_invoice_amount != nullptr)
      {
        amount_str = lv_textarea_get_text(ta_invoice_amount);
      }
      if (strlen(amount_str) == 0)
        amount_str = "100"; // Default to 100 if empty

      char currency_str[10] = "SAT"; // Default currency
      if (dd_invoice_currency != nullptr)
      {
        lv_dropdown_get_selected_str(dd_invoice_currency, currency_str, sizeof(currency_str));
      }

      USBSerial.printf("Invoice Amount: %s, Currency: %s\n", amount_str, currency_str);

      String apiUrl_invoice = "https://getsafebox.app/safebox/invoice";
      // Construct JSON payload dynamically
      String jsonPayload_invoice_str = "{\"amount\": \"" + String(amount_str) +
                                       "\", \"currency\": \"" + String(currency_str) +
                                       "\", \"comment\": \"Invoice from ESP32\"}";

      USBSerial.println("Connecting to invoice API: " + apiUrl_invoice);
      USBSerial.println("Payload: " + jsonPayload_invoice_str);
      http_invoice.begin(apiUrl_invoice);

      http_invoice.addHeader("Content-Type", "application/json");
      http_invoice.addHeader("Accept", "application/json");
      USBSerial.println("Using Cookie for invoice: " + accessTokenCookie);
      http_invoice.addHeader("Cookie", accessTokenCookie.c_str());

      USBSerial.println("Sending POST request for invoice with payload...");
      int httpCodeInvoice = http_invoice.POST(jsonPayload_invoice_str);

      if (httpCodeInvoice > 0)
      {
        USBSerial.printf("[HTTP Invoice] POST response code: %d\n", httpCodeInvoice);
        String responsePayloadInvoice = http_invoice.getString();
        USBSerial.println("[HTTP Invoice] Response payload:");
        USBSerial.println(responsePayloadInvoice);

        if (httpCodeInvoice == HTTP_CODE_OK || httpCodeInvoice == HTTP_CODE_CREATED)
        {
          String invoiceKey = "\"invoice\":\"";
          int startIndex = responsePayloadInvoice.indexOf(invoiceKey);
          if (startIndex != -1)
          {
            startIndex += invoiceKey.length();
            int endIndex = responsePayloadInvoice.indexOf("\"", startIndex);
            if (endIndex != -1)
            {
              String invoiceData = responsePayloadInvoice.substring(startIndex, endIndex);
              USBSerial.print("Extracted invoice: ");
              USBSerial.println(invoiceData);
              show_qr_code(invoiceData.c_str());
            }
            else
            {
              USBSerial.println("Could not find closing quote for invoice value.");
              show_qr_code("Parse Err: No End Quote");
            }
          }
          else
          {
            USBSerial.println("Invoice key not found in response.");
            show_qr_code("Parse Err: No Key");
          }
        }
        else
        {
          String errorMsg = "API Error: " + String(httpCodeInvoice);
          show_qr_code(errorMsg.c_str());
        }
      }
      else
      {
        USBSerial.printf("[HTTP Invoice] POST request failed, error: %s\n", http_invoice.errorToString(httpCodeInvoice).c_str());
        show_qr_code("HTTP Conn Error");
      }
      http_invoice.end();
    }
    else
    {
      USBSerial.println("WiFi not connected. Cannot make API call.");
      show_qr_code("WiFi Error");
    }
  }
}

static void ta_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);
  if (code == LV_EVENT_FOCUSED)
  {
    if (kb != NULL)
    {
      lv_keyboard_set_textarea(kb, ta);            // Assign the text area to the keyboard
      lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);   // Show the keyboard
      lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0); // Align keyboard to bottom
    }
  }
  else if (code == LV_EVENT_DEFOCUSED)
  {
    if (kb != NULL && lv_keyboard_get_textarea(kb) == ta)
    { // Hide only if it was for this ta
      // lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN); // Hide the keyboard
    }
  }
  else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
  { // User pressed Enter or Esc on physical keyboard (if any) or virtual keyboard action
    if (kb != NULL)
    {
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN); // Hide the keyboard
    }
    lv_obj_clear_state(ta, LV_STATE_FOCUSED); // Manually remove focus
    lv_indev_reset(NULL, ta);                 // Ensure input device is reset
  }
}

// Event handler for the keyboard itself (OK/Close)
static void kb_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *kb_target = lv_event_get_target(e); // This is the keyboard
  lv_obj_t *current_ta = lv_keyboard_get_textarea(kb_target);

  if (code == LV_EVENT_READY)
  { // "Ok" or "Enter" key on virtual keyboard
    USBSerial.println("Keyboard OK pressed");
    if (kb != NULL)
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (current_ta != NULL)
    {
      lv_obj_clear_state(current_ta, LV_STATE_FOCUSED); // Defocus the text area
      lv_indev_reset(NULL, current_ta);
    }
  }
  else if (code == LV_EVENT_CANCEL)
  { // "Close" key on virtual keyboard
    USBSerial.println("Keyboard Close pressed");
    if (kb != NULL)
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (current_ta != NULL)
    {
      lv_obj_clear_state(current_ta, LV_STATE_FOCUSED); // Defocus the text area
      lv_indev_reset(NULL, current_ta);
    }
  }
}

// Event handler for the login button on the info page
static void btn_login_info_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CLICKED)
  {
    if (ta_access_key == NULL || login_status_label == NULL)
      return;

    const char *access_key_from_input = lv_textarea_get_text(ta_access_key);
    if (strlen(access_key_from_input) > 0)
    {
      USBSerial.print("Attempting login with key from input: ");
      USBSerial.println(access_key_from_input);
      lv_label_set_text(login_status_label, "Logging in...");
      lv_obj_invalidate(login_status_label);
      lv_timer_handler();
      delay(50);

      if (perform_login_and_store_token(access_key_from_input))
      {
        USBSerial.println("Login successful via info page button. Rebuilding UI.");
        create_main_app_ui();
      }
      else
      {
        USBSerial.println("Login failed via info page button.");
        lv_label_set_text(login_status_label, "Login Failed.\nCheck key or connection.");
        // Optionally, ensure QR page button remains hidden if login fails
        if (btn_generate_invoice != nullptr)
        {
          lv_obj_add_flag(btn_generate_invoice, LV_OBJ_FLAG_HIDDEN);
        }
        show_qr_code("Login Failed. Please try again.");
      }
    }
    else
    {
      USBSerial.println("Access key input field is empty.");
      lv_label_set_text(login_status_label, "Access key field is empty.");
    }
  }
  // This is to ensure the keyboard is hidden after login button click
  if (kb != NULL)
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void btn_generate_ecash_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CLICKED)
  {
    USBSerial.println("Generate eCash Button Clicked.");

    // Hide keyboard if it was open for the amount input
    if (kb != NULL && lv_keyboard_get_textarea(kb) == ta_ecash_amount)
    {
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_state(ta_ecash_amount, LV_STATE_FOCUSED);
      lv_indev_reset(NULL, ta_ecash_amount);
    }

    // Clear previous eCash QR/status
    if (ecash_qr_widget != nullptr)
    {
      lv_obj_del(ecash_qr_widget);
      ecash_qr_widget = nullptr;
    }
    if (ecash_page_status_label != nullptr)
    {
      lv_obj_del(ecash_page_status_label);
      ecash_page_status_label = nullptr;
    }

    // Show a "Processing..." message
    ecash_page_status_label = lv_label_create(page_ecash);
    lv_label_set_text(ecash_page_status_label, "Processing eCash request...");
    lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60); // Position it where QR will be
    lv_timer_handler();
    delay(50);

    if (WiFi.status() != WL_CONNECTED)
    {
      USBSerial.println("[eCash] WiFi not connected.");
      lv_label_set_text(ecash_page_status_label, "Error: WiFi not connected.");
      return;
    }

    Preferences preferences;
    preferences.begin("app_prefs", true); // Read-only
    String accessTokenCookie = preferences.getString("accessToken", "");
    preferences.end();

    if (accessTokenCookie.length() == 0)
    {
      USBSerial.println("[eCash] Access token not found.");
      lv_label_set_text(ecash_page_status_label, "Error: Login Required.");
      return;
    }

    const char *amount_sats_str = "0";
    if (ta_ecash_amount != nullptr)
    {
      amount_sats_str = lv_textarea_get_text(ta_ecash_amount);
    }
    if (strlen(amount_sats_str) == 0 || String(amount_sats_str).toInt() <= 0)
    {
      USBSerial.println("[eCash] Invalid amount.");
      lv_label_set_text(ecash_page_status_label, "Error: Invalid amount.\nPlease enter a positive number of sats.");
      return;
    }

    USBSerial.printf("[eCash] Amount in sats: %s\n", amount_sats_str);

    HTTPClient http_ecash;
    String ecashApiUrl = "https://getsafebox.app/safebox/issueecash";
    String jsonPayload = "{\"amount\": \"" + String(amount_sats_str) + "\"}";

    USBSerial.println("[eCash] Calling API: " + ecashApiUrl);
    USBSerial.println("[eCash] Payload: " + jsonPayload);

    http_ecash.begin(ecashApiUrl);
    http_ecash.addHeader("Content-Type", "application/json");
    http_ecash.addHeader("Accept", "application/json");
    http_ecash.addHeader("Cookie", accessTokenCookie.c_str());

    int httpCode = http_ecash.POST(jsonPayload);

    if (httpCode > 0)
    {
      USBSerial.printf("[eCash] HTTP POST response code: %d\n", httpCode);
      String responsePayload = http_ecash.getString();
      USBSerial.println("[eCash] Response payload:");
      USBSerial.println(responsePayload);

      // Clear "Processing..." message before showing result
      if (ecash_page_status_label != nullptr)
      {
        lv_obj_del(ecash_page_status_label);
        ecash_page_status_label = nullptr;
      }

      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED)
      {
        // Attempt to parse JSON - very basic parsing
        String detailKey = "\"detail\":\"";
        int startIndex = responsePayload.indexOf(detailKey);
        if (startIndex != -1)
        {
          startIndex += detailKey.length();
          int endIndex = responsePayload.indexOf("\"", startIndex);
          if (endIndex != -1)
          {
            String ecashTokenDetail = responsePayload.substring(startIndex, endIndex);
            USBSerial.println("[eCash] Extracted eCash Detail: " + ecashTokenDetail);

            // Display as QR code
            ecash_qr_widget = lv_qrcode_create(page_ecash, 300, lv_color_black(), lv_color_white()); // Adjust size as needed
            if (ecash_qr_widget)
            {
              lv_qrcode_update(ecash_qr_widget, ecashTokenDetail.c_str(), ecashTokenDetail.length());
              lv_obj_align(ecash_qr_widget, LV_ALIGN_CENTER, 0, 50); // Position it below button
            }
            else
            {
              USBSerial.println("[eCash] Failed to create QR code object.");
              ecash_page_status_label = lv_label_create(page_ecash);
              lv_label_set_text(ecash_page_status_label, "Error: QR Gen Failed.");
              lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60);
            }
          }
          else
          {
            USBSerial.println("[eCash] Parse Error: No closing quote for detail.");
            ecash_page_status_label = lv_label_create(page_ecash);
            lv_label_set_text(ecash_page_status_label, "Error: API Response Parse Failed (end quote).");
            lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60);
          }
        }
        else
        {
          USBSerial.println("[eCash] Parse Error: 'detail' key not found.");
          ecash_page_status_label = lv_label_create(page_ecash);
          lv_label_set_text(ecash_page_status_label, "Error: API Response Parse Failed (no key).");
          lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60);
        }
      }
      else
      {
        ecash_page_status_label = lv_label_create(page_ecash);
        lv_label_set_text_fmt(ecash_page_status_label, "API Error: %d", httpCode);
        lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60);
      }
    }
    else
    {
      USBSerial.printf("[eCash] HTTP POST request failed, error: %s\n", http_ecash.errorToString(httpCode).c_str());
      if (ecash_page_status_label != nullptr)
      { // Update existing "Processing" label
        lv_label_set_text(ecash_page_status_label, "Error: HTTP Connection Failed.");
      }
      else
      { // Or create new if it was somehow deleted
        ecash_page_status_label = lv_label_create(page_ecash);
        lv_label_set_text(ecash_page_status_label, "Error: HTTP Connection Failed.");
        lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60);
      }
    }
    http_ecash.end();
  }
}

static void btn_pay_address_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CLICKED)
  {
    USBSerial.println("Pay Address Button Clicked.");

    if (kb != NULL)
    {
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(pay_address_status_label, "Processing payment...");
    lv_timer_handler();
    delay(50);

    if (WiFi.status() != WL_CONNECTED)
    {
      USBSerial.println("[PayAddress] WiFi not connected.");
      lv_label_set_text(pay_address_status_label, "Error: WiFi not connected.");
      return;
    }

    Preferences preferences;
    preferences.begin("app_prefs", true); // Read-only
    String accessTokenCookie = preferences.getString("accessToken", "");
    preferences.end();

    if (accessTokenCookie.length() == 0)
    {
      USBSerial.println("[PayAddress] Access token not found.");
      lv_label_set_text(pay_address_status_label, "Error: Login Required.");
      return;
    }

    const char *address = lv_textarea_get_text(ta_pay_address_address);
    const char *amount_str = lv_textarea_get_text(ta_pay_address_amount);
    const char *comment = lv_textarea_get_text(ta_pay_address_comment);
    char currency_str[10] = "SAT";
    lv_dropdown_get_selected_str(dd_pay_address_currency, currency_str, sizeof(currency_str));

    if (strlen(address) == 0)
    {
      lv_label_set_text(pay_address_status_label, "Error: Address is required.");
      return;
    }

    long amount_val = 0;
    if (strlen(amount_str) > 0)
    {
      amount_val = atol(amount_str);
    }

    HTTPClient http_pay;
    String payApiUrl = "https://getsafebox.app/safebox/payaddress";

    String jsonPayload = "{\"address\": \"" + String(address) + "\", " +
                         "\"amount\": " + String(amount_val) + ", " +
                         "\"currency\": \"" + String(currency_str) + "\", " +
                         "\"comment\": \"" + String(comment) + "\"}";

    USBSerial.println("[PayAddress] Calling API: " + payApiUrl);
    USBSerial.println("[PayAddress] Payload: " + jsonPayload);

    http_pay.begin(payApiUrl);
    http_pay.addHeader("Content-Type", "application/json");
    http_pay.addHeader("Accept", "application/json");
    http_pay.addHeader("Cookie", accessTokenCookie.c_str());

    int httpCode = http_pay.POST(jsonPayload);
    String responsePayload = http_pay.getString();

    USBSerial.printf("[PayAddress] HTTP POST response code: %d\n", httpCode);
    USBSerial.println("[PayAddress] Response payload:");
    USBSerial.println(responsePayload);

    if (httpCode > 0)
    {
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED)
      {
        lv_label_set_text(pay_address_status_label, "Payment request sent successfully!");
      }
      else
      {
        String errorMsg = "API Error: " + String(httpCode);
        lv_label_set_text(pay_address_status_label, errorMsg.c_str());
      }
    }
    else
    {
      USBSerial.printf("[PayAddress] HTTP POST request failed, error: %s\n", http_pay.errorToString(httpCode).c_str());
      lv_label_set_text(pay_address_status_label, "Error: HTTP Connection Failed.");
    }
    http_pay.end();
  }
}

void save_wifi_credentials(const char *ssid, const char *password)
{
  preferences.begin("wifi-creds", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  USBSerial.println("WiFi credentials saved.");
}

void attempt_saved_wifi_connection()
{
  if (preferences.begin("wifi-creds", true))
  { // Read-only
    String stored_ssid = preferences.getString("ssid", "");
    String stored_password = preferences.getString("password", "");
    preferences.end();

    if (stored_ssid.length() > 0)
    {
      USBSerial.print("Attempting to connect to saved WiFi: ");
      USBSerial.println(stored_ssid);
      WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20)
      { // Try for 10 seconds
        delay(500);
        USBSerial.print(".");
        attempts++;
      }
      if (WiFi.status() == WL_CONNECTED)
      {
        USBSerial.println("\nConnected to WiFi!");
        USBSerial.print("IP Address: ");
        USBSerial.println(WiFi.localIP());
        net_flag = 1;
        // configTime((const long)(8 * 3600), 0, ntpServer); // Configure NTP if needed
      }
      else
      {
        USBSerial.println("\nFailed to connect to saved WiFi.");
        WiFi.disconnect(true); // Ensure disconnected
        net_flag = 0;
      }
    }
    else
    {
      USBSerial.println("No saved WiFi credentials found.");
      net_flag = 0;
    }
  }
  else
  {
    USBSerial.println("Failed to open preferences for reading.");
    net_flag = 0;
  }
}

static void wifi_connect_event_handler(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *obj = lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED)
  {
    char selected_ssid[64];
    lv_dropdown_get_selected_str(wifi_ssid_dropdown, selected_ssid, sizeof(selected_ssid));
    const char *password = lv_textarea_get_text(wifi_password_ta);

    Serial.printf("Selected SSID: %s\n", selected_ssid);
    USBSerial.printf("Entered Password: %s\n", password);

    if (strlen(selected_ssid) == 0 || strcmp(selected_ssid, "Scanning...") == 0 || strcmp(selected_ssid, "No networks") == 0)
    {
      lv_label_set_text(wifi_status_label, "Please select a network.");
      return;
    }

    USBSerial.print("Connecting to: ");
    USBSerial.println(selected_ssid);
    lv_label_set_text(wifi_status_label, "Connecting...");

    WiFi.disconnect(true); // Disconnect from any previous attempt
    WiFi.begin(selected_ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30)
    { // Try for 15 seconds
      delay(500);
      USBSerial.print(".");
      lv_label_set_text_fmt(wifi_status_label, "Connecting (Attempt %d)", attempts + 1);
      lv_timer_handler(); // Keep LVGL responsive
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      USBSerial.println("\nConnected!");
      USBSerial.print("IP Address: ");
      USBSerial.println(WiFi.localIP());
      lv_label_set_text_fmt(wifi_status_label, "Connected!\nIP: %s", WiFi.localIP().toString().c_str());
      save_wifi_credentials(selected_ssid, password);
      net_flag = 1;
      // configTime((const long)(8 * 3600), 0, ntpServer);

      // Optionally, hide or delete the login screen and show the main UI
      if (wifi_login_screen != nullptr)
      {
        lv_obj_del_async(wifi_login_screen); // Use async delete
        wifi_login_screen = nullptr;
      }
      create_main_app_ui(); // Load the main application UI
      USBSerial.println("Main application UI loaded after WiFi connection.");
    }
    else
    {
      USBSerial.println("\nFailed to connect.");
      lv_label_set_text(wifi_status_label, "Failed. Check password/network.");
      WiFi.disconnect(true);
      net_flag = 0;
    }

    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  }
}

// Event handler for the password text area to show/hide the keyboard
static void password_ta_event_handler(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);
  // lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e); // If keyboard is passed as user data

  if (code == LV_EVENT_FOCUSED)
  {
    if (kb == nullptr)
    {                                                  // Create keyboard if it doesn't exist
      kb = lv_keyboard_create(lv_scr_act());           // Create on the current screen
      lv_obj_set_size(kb, LV_HOR_RES, LV_VER_RES / 2); // Adjust size as needed
                                                       // lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0); // Align to bottom
    }
    lv_keyboard_set_textarea(kb, ta);          // Link keyboard to this text area
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN); // Make sure keyboard is visible
    lv_obj_move_foreground(kb);                // Bring keyboard to front
  }

  if (code == LV_EVENT_DEFOCUSED)
  {
    if (kb != nullptr)
    {
      lv_keyboard_set_textarea(kb, NULL); // Unlink keyboard
      lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void create_wifi_login_ui()
{
  if (wifi_login_screen != nullptr)
  {
    lv_obj_clean(wifi_login_screen);
  }
  else
  {
    wifi_login_screen = lv_obj_create(NULL);
  }
  lv_scr_load(wifi_login_screen);

  lv_obj_t *title_label = lv_label_create(wifi_login_screen);
  lv_label_set_text(title_label, "WiFi Login");
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *ssid_label = lv_label_create(wifi_login_screen);
  lv_label_set_text(ssid_label, "Network (SSID):");
  lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 10, 40);

  wifi_ssid_dropdown = lv_dropdown_create(wifi_login_screen);
  lv_obj_set_width(wifi_ssid_dropdown, MY_HOR_RES - 20);
  lv_obj_align_to(wifi_ssid_dropdown, title_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 50);
  lv_dropdown_set_options(wifi_ssid_dropdown, "Scanning...");

  lv_obj_t *password_label = lv_label_create(wifi_login_screen);
  lv_label_set_text(password_label, "Password:");
  lv_obj_align_to(password_label, wifi_ssid_dropdown, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

  wifi_password_ta = lv_textarea_create(wifi_login_screen);
  lv_obj_set_width(wifi_password_ta, MY_HOR_RES - 20);
  lv_textarea_set_one_line(wifi_password_ta, true);
  lv_textarea_set_password_mode(wifi_password_ta, true);
  lv_obj_align_to(wifi_password_ta, password_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  lv_textarea_set_placeholder_text(wifi_password_ta, "Password");
  lv_obj_add_event_cb(wifi_password_ta, password_ta_event_handler, LV_EVENT_ALL, NULL);

  wifi_connect_btn = lv_btn_create(wifi_login_screen);
  lv_obj_align_to(wifi_connect_btn, wifi_password_ta, LV_ALIGN_OUT_BOTTOM_MID, 0, 15); // Adjust Y offset if keyboard overlaps
  lv_obj_add_event_cb(wifi_connect_btn, wifi_connect_event_handler, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_label = lv_label_create(wifi_connect_btn);
  lv_label_set_text(btn_label, "Connect");
  lv_obj_center(btn_label);

  wifi_status_label = lv_label_create(wifi_login_screen);
  lv_label_set_text(wifi_status_label, "");
  lv_obj_set_width(wifi_status_label, MY_HOR_RES - 20);
  lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(wifi_status_label, wifi_connect_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);

  USBSerial.println("Scanning for WiFi networks...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  USBSerial.println("Scan done.");

  if (n == 0)
  {
    USBSerial.println("No networks found.");
    lv_dropdown_set_options(wifi_ssid_dropdown, "No networks");
  }
  else
  {
    USBSerial.print(n);
    USBSerial.println(" networks found.");
    String ssid_list = "";
    for (int i = 0; i < n; ++i)
    {
      ssid_list += WiFi.SSID(i);
      if (i < n - 1)
      {
        ssid_list += "\n";
      }
    }
    lv_dropdown_set_options(wifi_ssid_dropdown, ssid_list.c_str());
  }
}

void create_main_app_ui()
{
  USBSerial.println("Loading main application UI...");
  // Example:
  lv_obj_clean(lv_scr_act()); // Or clean the current default screen if not creating a new one

  tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 0);

  // --- Populate Page 1 (Information Page with Login Input) ---
  page_info = lv_tabview_add_tab(tabview, "Account Info");

  // Create refresh button
  btn_refresh_info = lv_btn_create(page_info);
  lv_obj_align(btn_refresh_info, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_event_cb(btn_refresh_info, btn_refresh_info_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *label_refresh_info = lv_label_create(btn_refresh_info);
  lv_label_set_text(label_refresh_info, LV_SYMBOL_REFRESH);
  lv_obj_center(label_refresh_info);

  // Create keyboard (parented to screen to overlay tabs, initially hidden)
  kb = lv_keyboard_create(lv_scr_act());
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_ALL, NULL); // For OK/Close events on keyboard

  // --- Populate Page 2 (QR Code Page with Button) ---
  page_qr = lv_tabview_add_tab(tabview, "QR Code");

  // Create Amount Input Text Area on page_qr
  lv_obj_t *amount_label = lv_label_create(page_qr);
  lv_label_set_text(amount_label, "Amount:");
  lv_obj_align(amount_label, LV_ALIGN_TOP_LEFT, 10, 15); // Adjusted Y for a bit more space

  ta_invoice_amount = lv_textarea_create(page_qr);
  lv_obj_set_width(ta_invoice_amount, lv_pct(30)); // Made amount text area smaller
  lv_textarea_set_one_line(ta_invoice_amount, true);
  lv_textarea_set_placeholder_text(ta_invoice_amount, "100"); // Shorter placeholder
  lv_obj_align_to(ta_invoice_amount, amount_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
  lv_obj_add_event_cb(ta_invoice_amount, ta_event_cb, LV_EVENT_ALL, NULL);

  // Create Currency Dropdown on page_qr, to the right of amount
  lv_obj_t *currency_label = lv_label_create(page_qr);
  lv_label_set_text(currency_label, "Curr:");                                        // Shorter label
  lv_obj_align_to(currency_label, ta_invoice_amount, LV_ALIGN_OUT_RIGHT_MID, 10, 0); // Align to right of amount text area

  dd_invoice_currency = lv_dropdown_create(page_qr);
  lv_dropdown_set_options(dd_invoice_currency,
                          "SAT\n"
                          "CAD\n"
                          "USD\n"
                          "EUR\n"
                          "GBP\n"
                          "JPY\n"
                          "INR\n"
                          "CHF\n"
                          "AUD");
  lv_dropdown_set_selected(dd_invoice_currency, 0);  // Default to SAT
  lv_obj_set_width(dd_invoice_currency, lv_pct(30)); // Made dropdown smaller
  lv_obj_align_to(dd_invoice_currency, currency_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

  // Create the "Generate Invoice" button and its label, positioned below the amount/currency row and centered
  btn_generate_invoice = lv_btn_create(page_qr);
  lv_obj_set_width(btn_generate_invoice, lv_pct(60));
  // Align below the amount_label, but use LV_ALIGN_OUT_BOTTOM_MID to center it horizontally relative to the page_qr or a wider element.
  // For better centering, let's align it to the page_qr itself, below the inputs.
  lv_obj_align(btn_generate_invoice, LV_ALIGN_TOP_MID, 0, 55); // Adjust Y offset (55) as needed to clear inputs
  lv_obj_add_event_cb(btn_generate_invoice, btn_event_cb, LV_EVENT_CLICKED, NULL);

  label_generate_invoice = lv_label_create(btn_generate_invoice);
  lv_label_set_text(label_generate_invoice, "Generate Invoice");
  lv_obj_center(label_generate_invoice);

  // --- Populate Page 3 (eCash Generation) ---
  page_ecash = lv_tabview_add_tab(tabview, "eCash");

  lv_obj_t *ecash_amount_label = lv_label_create(page_ecash);
  lv_label_set_text(ecash_amount_label, "Amount (sats):");
  lv_obj_align(ecash_amount_label, LV_ALIGN_TOP_LEFT, 10, 15);

  ta_ecash_amount = lv_textarea_create(page_ecash);
  lv_obj_set_width(ta_ecash_amount, lv_pct(50));
  lv_textarea_set_one_line(ta_ecash_amount, true);
  lv_textarea_set_placeholder_text(ta_ecash_amount, "Enter sats amount");
  lv_obj_align_to(ta_ecash_amount, ecash_amount_label, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
  lv_obj_add_event_cb(ta_ecash_amount, ta_event_cb, LV_EVENT_ALL, NULL); // Reuse keyboard handler

  btn_generate_ecash = lv_btn_create(page_ecash);
  lv_obj_set_width(btn_generate_ecash, lv_pct(60));
  lv_obj_align(btn_generate_ecash, LV_ALIGN_TOP_MID, 15, 55); // Centered horizontally, adjust Y as needed
  lv_obj_add_event_cb(btn_generate_ecash, btn_generate_ecash_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *label_btn_ecash = lv_label_create(btn_generate_ecash);
  lv_label_set_text(label_btn_ecash, "Pay with eCash");
  lv_obj_center(label_btn_ecash);

  // Initial status/QR area message for eCash page
  ecash_page_status_label = lv_label_create(page_ecash);
  lv_label_set_text(ecash_page_status_label, "Enter amount and click generate.");
  lv_obj_set_width(ecash_page_status_label, lv_pct(90));
  lv_obj_set_style_text_align(ecash_page_status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ecash_page_status_label, LV_ALIGN_CENTER, 0, 60); // Position below button

  // --- Populate Page 4 (Pay Address) ---
  page_pay_address = lv_tabview_add_tab(tabview, "Pay Address");

  // Address Label and TextArea
  lv_obj_t *label_address = lv_label_create(page_pay_address);
  lv_label_set_text(label_address, "Address (Invoice, LNURL):");
  lv_obj_align(label_address, LV_ALIGN_TOP_LEFT, 10, 5);

  ta_pay_address_address = lv_textarea_create(page_pay_address);
  lv_obj_set_width(ta_pay_address_address, lv_pct(90));
  lv_textarea_set_one_line(ta_pay_address_address, true);
  lv_textarea_set_placeholder_text(ta_pay_address_address, "lnbc...");
  lv_obj_align_to(ta_pay_address_address, label_address, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  lv_obj_add_event_cb(ta_pay_address_address, ta_event_cb, LV_EVENT_ALL, NULL);

  ta_pay_address_amount = lv_textarea_create(page_pay_address);
  lv_obj_set_width(ta_pay_address_amount, lv_pct(90));
  lv_textarea_set_one_line(ta_pay_address_amount, true);
  lv_textarea_set_placeholder_text(ta_pay_address_amount, "Amount");
  lv_obj_align_to(ta_pay_address_amount, ta_pay_address_address, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  lv_obj_add_event_cb(ta_pay_address_amount, ta_event_cb, LV_EVENT_ALL, NULL);

  dd_pay_address_currency = lv_dropdown_create(page_pay_address);
  lv_dropdown_set_options(dd_pay_address_currency, "SAT\nUSD\nEUR\nGBP");
  lv_obj_set_width(dd_pay_address_currency, lv_pct(90));
  lv_obj_align_to(dd_pay_address_currency, ta_pay_address_amount, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);

  ta_pay_address_comment = lv_textarea_create(page_pay_address);
  lv_obj_set_width(ta_pay_address_comment, lv_pct(90));
  lv_textarea_set_one_line(ta_pay_address_comment, true);
  lv_textarea_set_placeholder_text(ta_pay_address_comment, "Comment (optional)");
  lv_obj_align_to(ta_pay_address_comment, dd_pay_address_currency, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
  lv_obj_add_event_cb(ta_pay_address_comment, ta_event_cb, LV_EVENT_ALL, NULL);

  btn_pay_address = lv_btn_create(page_pay_address);
  lv_obj_set_width(btn_pay_address, lv_pct(50));
  lv_obj_align_to(btn_pay_address, ta_pay_address_comment, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
  lv_obj_add_event_cb(btn_pay_address, btn_pay_address_event_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *label_btn_pay_address = lv_label_create(btn_pay_address);
  lv_label_set_text(label_btn_pay_address, "Pay Address");
  lv_obj_center(label_btn_pay_address);

  // Status Label
  pay_address_status_label = lv_label_create(page_pay_address);
  lv_label_set_text(pay_address_status_label, "Enter details and press pay.");
  lv_obj_set_width(pay_address_status_label, lv_pct(90));
  lv_label_set_long_mode(pay_address_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(pay_address_status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(pay_address_status_label, btn_pay_address, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  // Check initial login state and set UI accordingly
  Preferences preferences_setup;
  preferences_setup.begin("app_prefs", true); // Read-only
  String accessTokenCookieSetup = preferences_setup.getString("accessToken", "");
  preferences_setup.end();

  if (accessTokenCookieSetup.length() == 0)
  {
    USBSerial.println("Setup: Not logged in.");
    create_login_form_on_info_page();
    show_qr_code("Log in via 'Login Key' tab.");
    if (btn_generate_invoice != nullptr)
    {
      lv_obj_add_flag(btn_generate_invoice, LV_OBJ_FLAG_HIDDEN); // Hide button
    }
    if (btn_refresh_info != nullptr)
    {
      lv_obj_add_flag(btn_refresh_info, LV_OBJ_FLAG_HIDDEN);
    }
  }
  else
  {
    USBSerial.println("Setup: Already logged in.");
    perform_safebox_access_check(accessTokenCookieSetup);
    show_qr_code("Logged In. Ready for invoice."); // Or your preferred initial QR for logged-in state
    if (btn_generate_invoice != nullptr)
    {
      lv_obj_clear_flag(btn_generate_invoice, LV_OBJ_FLAG_HIDDEN); // Ensure button is visible
    }
    if (btn_refresh_info != nullptr)
    {
      lv_obj_clear_flag(btn_refresh_info, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void setup(void)
{
  USBSerial.begin(115200);
  USBSerial.println("Arduino_GFX + LVGL QR Code example with Button and Touch");

  // Initialize I2C bus
  Wire.begin(IIC_SDA, IIC_SCL); // Ensure IIC_SDA and IIC_SCL are defined in pin_config.h

  expander = new EXAMPLE_CHIP_CLASS(TCA95xx_8bit,
                                    (i2c_port_t)0, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                    IIC_SCL, IIC_SDA); // Note: ui.ino uses IIC_SCL, IIC_SDA here.
  if (expander)
  {
    expander->init();
    expander->begin();
  }

  // Initialize GFX
  if (!gfx->begin())
  {
    USBSerial.println("gfx->begin() failed!");
    while (1)
      ;
  }
  gfx->Display_Brightness(255); // Keep original brightness
  gfx->fillScreen(WHITE);

  // Initialize Touch Controller (FT3x68 example)
  // Ensure TP_INT is defined in pin_config.h
  IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
  touch_controller.reset(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, Arduino_IIC_Touch_Interrupt));

  if (touch_controller)
  {
    while (touch_controller->begin() == false)
    {
      USBSerial.println("Touch controller initialization fail");
      delay(2000); // Wait and retry
    }
    USBSerial.println("Touch controller initialization successfully");
    // Set touch power mode if applicable (from ui.ino)
    touch_controller->IIC_Write_Device_State(touch_controller->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
                                             touch_controller->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
  }
  else
  {
    USBSerial.println("Failed to create touch controller object!");
  }

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, MY_HOR_RES * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = MY_HOR_RES;
  disp_drv.ver_res = MY_VER_RES;
  disp_drv.flush_cb = my_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  // Initialize LVGL input device for touch
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // Setup LVGL tick timer
  const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &example_increase_lvgl_tick,
      .name = "lvgl_tick"};
  esp_timer_handle_t lvgl_tick_timer = NULL;
  esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);

  WiFi.mode(WIFI_STA);
  attempt_saved_wifi_connection();

  if (WiFi.status() != WL_CONNECTED)
  {
    USBSerial.println("Not connected to WiFi. Showing login page.");
    create_wifi_login_ui();
  }
  else
  {
    USBSerial.println("Already connected to WiFi. Skipping login page.");
    create_main_app_ui();
  }

  USBSerial.println("Setup done");
}

void loop()
{
  lv_timer_handler(); // Let LVGL do its work (handle events, draw, etc.)

  delay(5); // Small delay to keep loop responsive
}