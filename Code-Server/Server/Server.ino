/*
 * Module   	  : TIC-RBT1
 * Membres  	  : [corde_t], [brouar_l], [kingki_n], [judea_d]
 * Date     	  : 22-05-2026
 * Description    : Serveur HTTP ESP32-CAM pour Robot-Tank.
 * Gere le streaming video FPV (OV2640) et le pilotage
 * des moteurs via signaux PWM vers le Dual ESC.
 * Mode Point d'Acces WiFi (AP) — connexion directe sans routeur.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"


// CONFIG WIFI (Mode AP — le robot crée son propre réseau)

const char* AP_SSID     = "Robot_Tank_pas_touche!";
const char* AP_PASSWORD = "12tank3490";


// BROCHES MOTEURS (PWM vers Dual ESC)
// Le dual ESC fonctionne en mode throttle + steering :
//   GPIO12 = throttle (vitesse : avancer / reculer)
//   GPIO13 = steering (direction : tourner gauche / droite)

#define PIN_THROTTLE 12   // GPIO12 → SIG1 ESC (vitesse)
#define PIN_STEERING 13   // GPIO13 → SIG2 ESC (direction)

// Paramètres PWM LEDC
#define PWM_FREQ       50    // 50 Hz — standard ESC
#define PWM_RESOLUTION 16    // 16 bits → valeurs 0..65535

// Valeurs PWM pour ESC standard (en µs convertis en ticks 16 bits à 50Hz)
#define ESC_NEUTRAL  4915   // 1500µs = neutre
#define ESC_FORWARD  6400   // ~1955µs = avant / droite max
#define ESC_BACKWARD 3400   // ~1040µs = arrière / gauche max


// PINOUT CAMERA ESP32-CAM (AI-Thinker)

#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

// Handles des deux serveurs HTTP
static httpd_handle_t stream_httpd  = NULL;  // Port 81 → flux vidéo
static httpd_handle_t control_httpd = NULL;  // Port 80 → commandes + page web


// INIT CAMERA

bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sscb_sda = CAM_PIN_SIOD;
    config.pin_sscb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count     = 2;
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 20;
        config.fb_count     = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAMERA] Erreur init : 0x%x\n", err);
        return false;
    }
    Serial.println("[CAMERA] OK");
    return true;
}


// COMMANDES MOTEURS


// Convertit un pourcentage (-100 à +100) en ticks PWM ESC
uint32_t pctToTick(int pct) {
    pct = constrain(pct, -100, 100);
    if (pct == 0) return ESC_NEUTRAL;
    if (pct > 0)
        return ESC_NEUTRAL + (uint32_t)((ESC_FORWARD  - ESC_NEUTRAL) * pct / 100);
    else
        return ESC_NEUTRAL - (uint32_t)((ESC_NEUTRAL  - ESC_BACKWARD) * (-pct) / 100);
}

// Arrête le tank : throttle et steering au neutre
void motorsStop() {
    ledcWrite(PIN_THROTTLE, ESC_NEUTRAL);
    ledcWrite(PIN_STEERING, ESC_NEUTRAL);
}

// Commande le tank en mode throttle + steering
// throttlePct : -100 (recule max) à +100 (avance max)
// steeringPct : -100 (gauche max) à +100 (droite max)
void motorsSet(int throttlePct, int steeringPct) {
    ledcWrite(PIN_THROTTLE, pctToTick(throttlePct));
    ledcWrite(PIN_STEERING, pctToTick(steeringPct));
}


// PAGE WEB — interface de pilotage

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Robot-Tank TIC-RBT1</title>
<style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    
    body {
        background: radial-gradient(circle at 50% -20%, #1e293b, #020617);
        color: #f8fafc;
        font-family: system-ui, -apple-system, sans-serif;
        display: flex; flex-direction: column; align-items: center;
        min-height: 100vh; padding: 24px 16px; gap: 24px;
    }
    
    h1 {
        font-size: 1.4rem; color: #e2e8f0; font-weight: 600; letter-spacing: 2px;
        text-shadow: 0 0 20px rgba(255, 255, 255, 0.1);
    }
    
    /* Effet Glassmorphism pour structurer la page */
    .glass-panel {
        background: rgba(255, 255, 255, 0.03);
        backdrop-filter: blur(16px);
        -webkit-backdrop-filter: blur(16px);
        border: 1px solid rgba(255, 255, 255, 0.05);
        border-radius: 16px;
        box-shadow: 0 4px 30px rgba(0, 0, 0, 0.3);
        padding: 24px;
        width: 100%; max-width: 640px;
        display: flex; flex-direction: column; align-items: center; gap: 16px;
    }

    #stream-wrap {
        width: 100%; background: #000; border-radius: 12px;
        overflow: hidden; aspect-ratio: 4/3; display: flex;
        align-items: center; justify-content: center;
        box-shadow: inset 0 0 20px rgba(255, 255, 255, 0.05);
    }
    
    #stream { width: 100%; height: 100%; object-fit: contain; }
    
    #status {
        font-size: 0.85rem; color: #94a3b8;
        background: rgba(0, 0, 0, 0.3); padding: 6px 14px; border-radius: 20px;
    }

    .controls {
        display: grid; grid-template-columns: repeat(3, 72px);
        grid-template-rows: repeat(3, 72px); gap: 12px; margin-top: 8px;
    }
    
    /* Boutons d'action */
    .btn {
        background: rgba(255, 255, 255, 0.05);
        border: 1px solid rgba(255, 255, 255, 0.1);
        border-radius: 14px; color: #f8fafc; font-size: 1.5rem;
        cursor: pointer; display: flex; align-items: center; justify-content: center;
        user-select: none; touch-action: manipulation; transition: all 0.2s ease;
        box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
    }
    
    .btn:hover {
        background: rgba(255, 255, 255, 0.08); transform: translateY(-2px);
    }
    
    .btn:active, .btn.on {
        background: rgba(56, 189, 248, 0.15);
        border-color: #38bdf8; color: #38bdf8;
        transform: translateY(1px);
        box-shadow: 0 0 15px rgba(56, 189, 248, 0.3);
    }
    
    .stop {
        background: rgba(239, 68, 68, 0.1); border-color: rgba(239, 68, 68, 0.2); color: #fca5a5;
    }
    
    .stop:active {
        background: rgba(239, 68, 68, 0.25); border-color: #ef4444; color: #fff;
        box-shadow: 0 0 15px rgba(239, 68, 68, 0.4);
    }

    /* Slider de vitesse épuré */
    .speed-row {
        display: flex; align-items: center; gap: 16px; font-size: 0.9rem;
        width: 100%; justify-content: center; margin-top: 12px; color: #cbd5e1;
    }
    
    #speed {
        width: 180px; accent-color: #38bdf8; cursor: pointer;
    }
    
    #spv { min-width: 40px; text-align: right; font-weight: 500; color: #38bdf8; }
    
    .info {
        font-size: 0.75rem; color: #64748b; text-align: center; margin-top: 8px;
    }
</style>
</head>
<body>
<h1>⬡ ROBOT-TANK TIC-RBT1</h1>

<div class="glass-panel">
    <div id="stream-wrap">
        <img id="stream" src="http://192.168.4.1:81/stream" alt="Flux FPV">
    </div>
    <div id="status">En attente…</div>
</div>

<div class="glass-panel">
    <div class="controls">
        <div></div>
        <button class="btn" onpointerdown="mv('forward')"  onpointerup="mv('stop')">▲</button>
        <div></div>
        <button class="btn" onpointerdown="mv('left')"     onpointerup="mv('stop')">◀</button>
        <button class="btn stop" onpointerdown="mv('stop')">■</button>
        <button class="btn" onpointerdown="mv('right')"    onpointerup="mv('stop')">▶</button>
        <div></div>
        <button class="btn" onpointerdown="mv('backward')" onpointerup="mv('stop')">▼</button>
        <div></div>
    </div>

    <div class="speed-row">
        <span>Vitesse</span>
        <input type="range" id="speed" min="10" max="100" value="80"
            oninput="document.getElementById('spv').textContent=this.value+'%'">
        <span id="spv">80%</span>
    </div>
</div>

<p class="info">Clavier : ↑ ↓ ← → ou Espace pour stop</p>

<script>
    const status = document.getElementById('status');
    const img    = document.getElementById('stream');

    const mv = (action) => {
        const spd = document.getElementById('speed').value;
        fetch('/action?go=' + action + '&speed=' + spd)
        .then(() => status.textContent = 'Commande : ' + action)
        .catch(() => status.textContent = 'Erreur réseau');
    };

    const keys = {
        ArrowUp:'forward', ArrowDown:'backward',
        ArrowLeft:'left',  ArrowRight:'right', ' ':'stop'
    };
    const held = {};
    document.addEventListener('keydown', e => {
        const c = keys[e.key];
        if(c && !held[e.key]){ held[e.key]=true; mv(c); e.preventDefault(); }
    });
    document.addEventListener('keyup', e => {
        if(keys[e.key]){ held[e.key]=false; mv('stop'); }
    });

    img.onerror = () => setTimeout(() => {
        img.src = 'http://192.168.4.1:81/stream?' + Date.now();
    }, 2000);
    img.onload = () => status.textContent = 'Flux vidéo actif';
</script>
</body>
</html>
)rawliteral";

// HANDLER — Page principale GET /

static esp_err_t indexHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}


// HANDLER — Commandes GET /action?go=forward&speed=80

static esp_err_t actionHandler(httpd_req_t *req) {
    char buf[64];
    char action[16] = {0};
    int  speed = 80;

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(buf, "go",    val, sizeof(val)) == ESP_OK)
            strncpy(action, val, sizeof(action) - 1);
        if (httpd_query_key_value(buf, "speed", val, sizeof(val)) == ESP_OK)
            speed = atoi(val);
    }

    Serial.printf("[ACTION] %s  speed=%d%%\n", action, speed);

    // Mode throttle + steering :
    // forward/backward → throttle bouge, steering neutre
    // left/right       → throttle neutre, steering bouge
    if      (strcmp(action, "forward")  == 0) motorsSet( speed,  0);      // Avancer
    else if (strcmp(action, "backward") == 0) motorsSet(-speed,  0);      // Reculer
    else if (strcmp(action, "left")     == 0) motorsSet( 0,     speed);  // Tourner gauche
    else if (strcmp(action, "right")    == 0) motorsSet( 0,    -speed);  // Tourner droite
    else                                      motorsStop();               // Stop

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "OK", 2);
}


// HANDLER — Stream MJPEG GET /stream (port 81)

static esp_err_t streamHandler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[STREAM] Erreur capture");
            res = ESP_FAIL;
            break;
        }

        res = httpd_resp_send_chunk(req, "\r\n--frame\r\n", 12);
        if (res != ESP_OK) { esp_camera_fb_return(fb); break; }

        size_t hlen = snprintf(part_buf, sizeof(part_buf),
            "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) { esp_camera_fb_return(fb); break; }

        res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return res;
}


// DÉMARRAGE SERVEURS HTTP

void startServers() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();

    // Serveur contrôle sur port 80
    cfg.server_port = 80;
    if (httpd_start(&control_httpd, &cfg) == ESP_OK) {
        httpd_uri_t ui  = { .uri = "/",       .method = HTTP_GET, .handler = indexHandler  };
        httpd_uri_t act = { .uri = "/action", .method = HTTP_GET, .handler = actionHandler };
        httpd_register_uri_handler(control_httpd, &ui);
        httpd_register_uri_handler(control_httpd, &act);
        Serial.println("[HTTP] Contrôle sur port 80");
    }

    // Serveur stream sur port 81
    cfg.server_port = 81;
    cfg.ctrl_port   = 32769;
    if (httpd_start(&stream_httpd, &cfg) == ESP_OK) {
        httpd_uri_t st = { .uri = "/stream", .method = HTTP_GET, .handler = streamHandler };
        httpd_register_uri_handler(stream_httpd, &st);
        Serial.println("[HTTP] Stream sur port 81");
    }
}


// SETUP

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Robot-Tank TIC-RBT1 ===");

    // Initialisation PWM pour le dual ESC (syntaxe ESP32 Core 3.0)
    ledcAttach(PIN_THROTTLE, PWM_FREQ, PWM_RESOLUTION);
    ledcAttach(PIN_STEERING, PWM_FREQ, PWM_RESOLUTION);

    // Signal neutre immédiat → arme les ESC sans faire bouger le tank
    motorsStop();
    Serial.println("[MOTORS] Signal neutre envoyé — armement ESC (2s)…");
    delay(2000);
    Serial.println("[MOTORS] ESC armé");

    // Initialisation de la caméra
    if (!initCamera()) {
        Serial.println("[ERREUR] Caméra KO — redémarrage dans 5s");
        delay(5000);
        ESP.restart();
    }

    // Création du réseau WiFi Point d'Accès
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WIFI] AP '%s' créé\n", AP_SSID);
    Serial.printf("[WIFI] Interface : http://%s\n", ip.toString().c_str());
    Serial.printf("[WIFI] Stream    : http://%s:81/stream\n", ip.toString().c_str());

    startServers();
    Serial.println("[READY] Robot prêt !\n");
}


// LOOP

void loop() {
    // Sécurité : stop moteurs si plus aucun client connecté
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 1000) {
        lastCheck = millis();
        if (WiFi.softAPgetStationNum() == 0) {
            motorsStop();
        }
    }
    delay(10);
}
