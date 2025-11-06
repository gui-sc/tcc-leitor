# TCC Leitor - ESP32-CAM com QR Code e Servo

Sistema de leitura de QR Code usando ESP32-CAM para controlar servos motores.

## 📋 Requisitos

- **Hardware:**
  - ESP32-CAM (modelo AI-Thinker)
  - Servo motor
  - Cabo USB para programação

- **Software:**
  - [Arduino IDE](https://www.arduino.cc/en/software) (versão 1.8 ou superior)
  - Driver USB-Serial (CH340/CP2102)

## 🔧 Instalação

### 1. Clone o repositório

```bash
git clone https://github.com/gui-sc/tcc-leitor.git
cd tcc-leitor
```

### 2. Configurar Arduino IDE

1. Abra a Arduino IDE
2. Vá em **Arquivo > Preferências**
3. Em "URLs Adicionais para Gerenciadores de Placas", adicione:
   ```
   https://dl.espressif.com/dl/package_esp32_index.json
   ```
4. Vá em **Ferramentas > Placa > Gerenciador de Placas**
5. Procure por "esp32" e instale **"esp32 by Espressif Systems"**

### 3. Instalar Bibliotecas

No Arduino IDE, vá em **Ferramentas > Gerenciar Bibliotecas** e instale:

- `ESP32QRCodeReader`
- `ESP32Servo`

### 4. Configurar o Projeto

1. Abra o arquivo `tcc-leitor.ino`
2. Configure suas credenciais Wi-Fi:
   ```cpp
   #define WIFI_SSID "seu_wifi"
   #define WIFI_PASS "sua_senha"
   ```

### 5. Configurar a Placa

Em **Ferramentas**, configure:

- **Placa:** "AI Thinker ESP32-CAM"
- **Porta:** Selecione a porta COM do seu ESP32-CAM
- **Upload Speed:** 115200

## 🚀 Executar o Projeto

1. Conecte o ESP32-CAM ao computador
2. Clique em **Upload** (seta para direita) na Arduino IDE
3. Aguarde a compilação e upload
4. Abra o **Monitor Serial** (Ctrl+Shift+M) com baud rate **115200**
5. Após a conexão Wi-Fi, o IP será exibido no monitor serial

## 🌐 Acessar a Interface Web

Após o upload, acesse no navegador:

```
http://[IP_DO_ESP32]
```

### Endpoints disponíveis:

- `/` - Interface web principal com streaming de vídeo
- `/stream` - Stream MJPEG da câmera
- `/jpg` - Captura uma foto única
- `/qr` - Retorna o último QR Code lido (JSON)

## 📱 Funcionamento

O sistema detecta QR Codes contendo:
- **"ORGANICO"** → Move o servo para a esquerda
- **"RECICLAVEL"** → Move o servo para a direita

## 🔌 Conexões de Hardware

- **Servo Motor:** Pino GPIO 2

## 📝 Notas

- O servo volta à posição neutra (95°) após 1.8 segundos
- Há um debounce de 2.5 segundos entre ações do mesmo QR Code
- A qualidade JPEG padrão está configurada em 10 (ajustável no código)
