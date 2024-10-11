# ESP32-C3 Control CAN Combinado

Este proyecto implementa un sistema de comunicación TWAI (CAN) en un ESP32-C3, combinando funcionalidades de configuración de ángulo de volante y monitoreo de mensajes CAN específicos. Incluye una interfaz web para control y visualización.

## Características

- Configura el ESP32-C3 como un punto de acceso WiFi.
- Implementa un servidor web con una interfaz de usuario interactiva.
- Permite la calibración del ángulo de volante mediante una secuencia específica.
- Monitorea y muestra mensajes CAN con ID 0x762 par ver el status de la configracion de angulo de volante.
- Envía tramas CAN predefinidas para configuración y diagnóstico.
- Incluye una cuenta regresiva de 30 segundos para la calibración del volante.
- Filtra y muestra mensajes CAN específicos (ID 0x762) con determinación de estado.

## Requisitos de Hardware

- Placa ESP32-C3
- Transceptor CAN compatible (por ejemplo, SN65HVD230)

## Configuración de Pines

- TWAI_TX_PIN: GPIO 18
- TWAI_RX_PIN: GPIO 19

## Configuración

1. Conecte el transceptor CAN a los pines GPIO 18 (TX) y 19 (RX) del ESP32-C3.
2. Compile y cargue el código en su ESP32-C3 utilizando el ESP-IDF.

## Uso

1. El ESP32-C3 creará un punto de acceso WiFi llamado "ESP32_AP" (sin contraseña).
2. Conéctese a esta red WiFi con su dispositivo.
3. Abra un navegador web y vaya a la dirección IP del ESP32-C3 (generalmente 192.168.4.1).
4. La interfaz web mostrará:
   - Un botón para iniciar la calibración del ángulo de volante.
   - Un botón para enviar mensajes CAN predefinidos.
   - Una lista de mensajes CAN recibidos y filtrados (ID 0x762).
5. Para calibrar el ángulo de volante:
   - Haga clic en "Start Countdown" y siga las instrucciones en pantalla.
   - Gire el volante completamente a la izquierda, luego a la derecha, y finalmente céntrelo.
6. Para comprobar el estado de la configuración, haga clic en el botón correspondiente.
7. Observe los mensajes recibidos y sus estados en la página web.

## Filtrado de Mensajes TWAI

El sistema filtra los mensajes TWAI con los siguientes criterios:
- ID del mensaje: 0x762
- Primer byte (índice 0): 0x23
- Segundo byte (índice 1): 0x00

## Determinación del Estado

El estado de cada mensaje filtrado se determina por el cuarto byte (índice 3):
- Si el nibble menos significativo es 0xC (por ejemplo, 0xEC, 0x6C, 0x8C), el estado es "Status 4"
- De lo contrario, el estado es "Status 3"

## Notas

- Este proyecto está configurado para una velocidad de CAN de 500 kbit/s.
- La interfaz web se actualiza automáticamente cada 5 segundos.
- Asegúrese de que su vehículo sea compatible con las tramas CAN enviadas por este dispositivo.

## Advertencia

Este proyecto interactúa directamente con el sistema de dirección del vehículo. Úselo bajo su propio riesgo y solo si entiende completamente las implicaciones y los riesgos asociados.

## Contribuciones

Las contribuciones a este proyecto son bienvenidas. Por favor, haga un fork del repositorio y envíe un pull request con sus cambios propuestos.

## Soporte

Para preguntas o soporte, por favor abra un issue en el repositorio de GitHub.