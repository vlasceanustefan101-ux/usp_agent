# TR-369 (USP) Controller în C++ cu RabbitMQ

Controller USP (TR-369, User Services Platform) scris în C++17. Comunică cu Agenții USP
(de ex. routerul **EX520**) prin **RabbitMQ**, folosind MTP-ul **STOMP** din specificație.
Mesajele USP sunt codate cu schemele protobuf oficiale Broadband Forum
(`proto/usp-record-1-4.proto`, `proto/usp-msg-1-4.proto`).

```
 ┌──────────────┐  STOMP  ┌──────────────────────────┐  STOMP  ┌──────────────────┐
 │ Router EX520 │ ──────► │ RabbitMQ (docker)        │ ◄────── │ usp-controller   │
 │ (USP Agent)  │ ◄────── │ /queue/usp.controller    │ ──────► │ (C++)            │
 └──────────────┘         │ /queue/usp.agent.<nume>  │         │  - consolă CLI   │
                          │ /queue/usp.controller.tasks ◄───── │  - API JSON      │
 backend / scripturi ───► │ (AMQP 5672 / STOMP 61613)│         └──────────────────┘
                          └──────────────────────────┘
```

## Ce face

- Se conectează la RabbitMQ prin STOMP 1.2 (heart-beat, reconectare automată, TLS opțional).
- Primește USP Records pe destinația proprie (`/queue/usp.controller`) și învață destinația fiecărui
  Agent din header-ul `reply-to-dest`, din `STOMPConnectRecord` sau din configurare.
- Răspunde automat cu `NotifyResp` la `Notify` (Boot!, ValueChange, OnBoardRequest etc.).
- La conectarea / pornirea unui Agent face automat un `Get` pe `Device.DeviceInfo.` (configurabil).
- Trimite orice cerere USP: Get, Set, Add, Delete, Operate, GetInstances, GetSupportedDM,
  GetSupportedProtocol și potrivește răspunsurile după `msg_id` (cu timeout).
- **Consolă interactivă** pentru teste manuale.
- **API JSON prin RabbitMQ** (`/queue/usp.controller.tasks`), ca un backend să poată trimite comenzi
  la routere, la fel cum ați face cu un ACS TR-069.

Testat local cu RabbitMQ 4.1 și cu **OB-USP-Agent** (agentul de referință al Broadband Forum):
Get, Set, Add (abonamente), Operate, GetInstances, GetSupportedDM, notificări Boot!, ValueChange și
OnBoardRequest.

## Structura

```
proto/                 schemele USP 1.4 (BBF) + API-ul JSON al controllerului
src/stomp/             client STOMP 1.2 (frame-uri, TCP/TLS, reconectare, heart-beat)
src/usp/               construire/parsare USP Record și USP Message
src/controller/        Controller (registru agenți, cereri/răspunsuri), API JSON, consolă
src/main.cpp           executabilul usp-controller
tools/usp_fake_agent.cpp  agent USP simulat, pentru teste fără router
tests/                 teste unitare
config/controller.conf configurarea controllerului
docker/rabbitmq/       configurare RabbitMQ (plugin STOMP + MQTT, utilizatori, cozi)
docker/obuspa/         OB-USP-Agent ca agent de test (opțional)
```

## Pornire rapidă (Docker)

Cerințe: Docker + Docker Compose.

```bash
docker compose up -d --build          # RabbitMQ + controller
docker compose logs -f controller     # log-urile controllerului
docker attach usp-controller          # consola interactivă (ieșire fără oprire: Ctrl-P Ctrl-Q)
```

Test fără router, cu agentul simulat:

```bash
docker compose --profile sim up -d
docker compose logs -f controller     # vezi Boot!, NotifyResp și Get-ul automat pe Device.DeviceInfo.
```

Test cu agentul de referință BBF (se compilează din sursă, durează câteva minute):

```bash
docker compose --profile obuspa up -d --build
```

Interfața RabbitMQ: http://localhost:15672 (utilizator `admin`, parola `admin-secret`).

> **Parolele din `docker/rabbitmq/definitions.json` și `config/controller.conf` sunt doar pentru test local.**
> Schimbă-le înainte de orice altă utilizare (hash-ul se generează cu `rabbitmqctl hash_password <parola>`).

| Utilizator RabbitMQ | Parola | Folosit de |
|---|---|---|
| `admin` | `admin-secret` | interfața web |
| `usp-controller` | `controller-secret` | controllerul |
| `usp-agent` | `agent-secret` | routerul / agenții de test |

## Compilare nativă (fără Docker)

Ubuntu/Debian:

```bash
sudo apt install build-essential cmake libprotobuf-dev protobuf-compiler libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/unit_tests
docker compose up -d rabbitmq         # doar brokerul
./build/usp-controller --config config/controller.conf
```

Orice cheie din config se poate suprascrie cu `--set cheie=valoare` sau cu variabile `USP_*`
(ex. `USP_STOMP_HOST`, `USP_STOMP_PASSWORD`, `USP_CONTROLLER_ENDPOINT_ID`, `USP_LOG_LEVEL=debug`).

## Test cu routerul EX520

1. **Rețea**: PC-ul cu Docker și routerul trebuie să se vadă. Află IP-ul PC-ului (ex. `192.168.1.10`) și
   verifică să nu blocheze firewall-ul portul **61613/tcp**.
2. **Configurează USP pe router.** Parametrii TR-181 de mai jos se pot seta din interfața web a
   routerului (dacă firmware-ul expune USP) sau **din ACS-ul vostru TR-069** cu `SetParameterValues`
   (dacă routerul suportă ambele protocoale, e cea mai simplă cale):

   | Parametru | Valoare |
   |---|---|
   | `Device.STOMP.Connection.1.Enable` | `true` |
   | `Device.STOMP.Connection.1.Host` | IP-ul PC-ului, ex. `192.168.1.10` |
   | `Device.STOMP.Connection.1.Port` | `61613` |
   | `Device.STOMP.Connection.1.Username` | `usp-agent` |
   | `Device.STOMP.Connection.1.Password` | `agent-secret` |
   | `Device.STOMP.Connection.1.VirtualHost` | `/` |
   | `Device.STOMP.Connection.1.EnableEncryption` | `false` (doar pentru test) |
   | `Device.STOMP.Connection.1.EnableHeartbeats` | `true` |
   | `Device.LocalAgent.MTP.1.Enable` | `true` |
   | `Device.LocalAgent.MTP.1.Protocol` | `STOMP` |
   | `Device.LocalAgent.MTP.1.STOMP.Reference` | `Device.STOMP.Connection.1` |
   | `Device.LocalAgent.MTP.1.STOMP.Destination` | `/queue/usp.agent.ex520` |
   | `Device.LocalAgent.Controller.1.Enable` | `true` |
   | `Device.LocalAgent.Controller.1.EndpointID` | `proto::usp-controller` (= `controller.endpoint_id`) |
   | `Device.LocalAgent.Controller.1.AssignedRole` | rolul „Full Access” (de obicei `Device.LocalAgent.ControllerTrust.Role.1`) |
   | `Device.LocalAgent.Controller.1.MTP.1.Enable` | `true` |
   | `Device.LocalAgent.Controller.1.MTP.1.Protocol` | `STOMP` |
   | `Device.LocalAgent.Controller.1.MTP.1.STOMP.Reference` | `Device.STOMP.Connection.1` |
   | `Device.LocalAgent.Controller.1.MTP.1.STOMP.Destination` | `/queue/usp.controller` |

   Același set de parametri, testat cu OB-USP-Agent, e în `docker/obuspa/factory_reset.txt`.
3. **Pornește** `docker compose up -d --build` și urmărește `docker compose logs -f controller`.
   Când routerul se conectează apare `USP: new agent os::XXXXXX-SERIAL` și răspunsul la Get-ul automat.
   În interfața RabbitMQ (tab-ul *Connections*) trebuie să vezi conexiunea STOMP a routerului.
4. **Comenzi** din consolă (`docker attach usp-controller`):

   ```
   usp> agents
   usp> use os::XXXXXX-SERIAL
   usp> get Device.DeviceInfo.
   usp> get Device.WiFi.SSID.*.SSID
   usp> gsdm Device.WiFi. -1
   usp> set Device.WiFi.SSID.1. SSID="Test USP"
   usp> sub Event Device.Boot!
   usp> sub ValueChange Device.WiFi.SSID.1.SSID
   usp> op Device.Reboot()
   ```

### Dacă nu merge

- `dropping Record ... addressed to '...'`: `Device.LocalAgent.Controller.1.EndpointID` de pe router
  nu e egal cu `controller.endpoint_id`.
- Routerul nu apare deloc: verifică în RabbitMQ *Connections* dacă s-a conectat. Dacă nu, verifică IP,
  port, utilizator/parolă și VirtualHost (`/`).
- Controllerul nu știe destinația routerului (`no STOMP destination known`): unele firmware-uri nu trimit
  header-ul `reply-to-dest`. Adaugă în `config/controller.conf`:
  `agent."os::XXXXXX-SERIAL" = /queue/usp.agent.ex520`.
- Pentru detalii: `USP_LOG_LEVEL=debug` afișează fiecare mesaj USP în JSON.

### Dacă EX520 suportă doar MQTT

Pluginul MQTT al RabbitMQ e deja activ (port **1883**). RabbitMQ mapează topicurile MQTT pe exchange-ul
`amq.topic`, iar STOMP le vede ca `/topic/...` (separatorul `/` devine `.`). Configurare posibilă:

- pe router: MQTT către `IP-PC:1883`, utilizator `usp-agent`, topicul agentului `usp/agent/ex520`,
  topicul controllerului `usp/controller`;
- pe controller: `controller.destination = /topic/usp.controller` și, dacă routerul nu trimite
  `MQTTConnectRecord`, `agent."os::XXXXXX-SERIAL" = /topic/usp.agent.ex520`.

Această variantă **nu a fost încă testată** cu un agent MQTT real; spune-mi dacă ai nevoie de ea și o validez.

## API JSON prin RabbitMQ

Backend-ul publică un `Task` (JSON, formatul din `proto/usp-controller-api.proto`) în coada
`usp.controller.tasks`. Rezultatul (`TaskResult`) vine în coada din proprietatea AMQP `reply_to`
(sau header-ul STOMP `reply-to`), altfel în `usp.controller.results`. `correlation_id` este păstrat.

```json
{"task_id": "t1", "endpoint_id": "os::XXXXXX-SERIAL",
 "request": {"get": {"param_paths": ["Device.DeviceInfo.SoftwareVersion"]}}}

{"task_id": "t2", "endpoint_id": "os::XXXXXX-SERIAL",
 "request": {"set": {"update_objs": [{"obj_path": "Device.WiFi.SSID.1.",
   "param_settings": [{"param": "SSID", "value": "Casa", "required": true}]}]}}}

{"task_id": "t3", "endpoint_id": "os::XXXXXX-SERIAL",
 "request": {"operate": {"command": "Device.Reboot()", "command_key": "r1", "send_resp": true}}}

{"task_id": "t4", "list_agents": true}
```

Rezultat:

```json
{"task_id": "t1", "endpoint_id": "os::XXXXXX-SERIAL", "status": "ok",
 "response": {"header": {"msg_id": "...", "msg_type": "GET_RESP"},
              "body": {"response": {"get_resp": {"req_path_results": [...]}}}}}
```

`status` poate fi `ok`, `usp_error` (Agentul a răspuns cu USP Error), `timeout` sau `error`.
Pentru un test rapid se poate publica din interfața web RabbitMQ: *Queues → usp.controller.tasks →
Publish message*, cu proprietatea `reply_to` = `usp.controller.results`.

## Limitări actuale

- Doar MTP STOMP (WebSocket și MQTT nativ nu sunt implementate; MQTT merge prin bridge-ul RabbitMQ).
- Nu suportă E2E Session Context (`session_context` records); se folosesc `no_session_context`,
  varianta uzuală.
- Registrul de agenți este în memorie (se reface la reconectarea agenților).
- TLS spre RabbitMQ e implementat (`stomp.tls = true`, port 61614), dar nu a fost testat încă.
