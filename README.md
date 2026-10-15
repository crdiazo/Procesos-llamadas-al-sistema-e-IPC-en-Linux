# Procesos-llamadas-al-sistema-e-IPC-en-Linux
Comprender experimentalmente cómo Linux crea y administra procesos y cómo procesos independientes se comunican utilizando mecanismos IPC proporcionados por el sistema operativo. 

**Asignatura:** Sistemas Operativos  
**Implementación:** Lenguaje C (C11 / POSIX.1-2008)  
**Mecanismos IPC desarrollados:** Tuberías Nombradas (FIFO) y Memoria Compartida POSIX con Semáforos POSIX  
**Grupo de trabajo:** `grupo1` (configurable mediante `make GRUPO=<nombre>`)

## Integrantes:
- Justin David Vargas Vasquez <jusvargasva@unal.edu.co> 
- Cristian Andres Diaz Ortega <crdiazo@unal.edu.co>
- Josue Gabriel Caviativa Cardenas <jcaviativa@unal.edu.co>
- Keller Joseph Guzman Ariza <kguzmana@unal.edu.co>
---

## 1. Estructura del repositorio
```
Practica1/
├── Makefile
├── LEEME.md
├── datos.csv              # dataset (ver sección 2)
├── comun/
│   ├── protocolo.h        # nombres IPC, códigos y estructuras de los mensajes
│   ├── utilidades.c/h     # write_all/read_all, apertura de FIFO, señales
│   ├── csv.c/h            # búsqueda secuencial en el CSV
│   ├── interfaz.c/h       # menú y salida del cliente
│   └── semaforos.c/h      # espera de semáforos vigilando al otro proceso
├── fifo/
│   ├── servidor_fifo.c
│   └── cliente_fifo.c
└── shm/
    ├── servidor_shm.c
    └── cliente_shm.c
```

Los ejecutables se generan en `bin/` y los objetos en `obj/`.
 
**Cómo se relacionan los módulos.** `csv.c` no sabe qué mecanismo IPC se usa: por cada registro que coincide invoca una función (callback) que le entrega el servidor. Los dos servidores reutilizan el mismo `csv.c` y los dos clientes el mismo `interfaz.c`, por lo que la funcionalidad visible es idéntica en ambas versiones.

--- 
## 2. Descripción del dataset
- **Origen:**
- **Archivo:** *datos.csv*
- **Tamaño:**
- **Separador:** coma, con encabezado en la primera línea, sin comillas ni comas dentro de los campos.


  
| Columna | Tipo en C | Formato / restricciones |
|---|---|---|
| `id` | `int64_t` | entero |
| `nombre` | `char[64]` | hasta 63 caracteres |
| `categoria` | `char[32]` | hasta 31 caracteres |
| `precio` | `double` | real ≥ 0, punto decimal |
| `stock` | `int32_t` | entero ≥ 0 |
| `fecha` | `char[11]` | `AAAA-MM-DD` |

**Criterios de búsqueda:**
 
1. Por categoría (igualdad exacta, sin distinguir mayúsculas).
2. Por rango de precio (`precio_min ≤ precio ≤ precio_max`).
3. Por nombre (subcadena, sin distinguir mayúsculas).
**Uso de memoria.** El CSV completo permanece en disco. En cada consulta el servidor abre el archivo con `fopen()`, lo lee línea a línea con `getline()` (un único búfer dinámico que se libera con `free()` al terminar) y envía cada coincidencia apenas la encuentra. La memoria del servidor no crece con el tamaño del archivo. 
 
**Límite de resultados.** Por defecto se muestran 25 resultados (`LIMITE_POR_DEFECTO`). La opción 4 del menú lo cambia (0 = sin límite). El servidor siempre recorre todo el archivo para informar el total de coincidencias.
 
---

## 3. Compilación y ejecución
 
**Requisitos:** Linux, `gcc` (C11), `make`, `glibc` con `librt` y `pthread`. Para las observaciones: `strace`, `lsof`, `psmisc` (`pstree`), `procps`.
 
```bash
make                    # compila los 4 ejecutables en bin/
make GRUPO=otro_nombre  # cambia el sufijo de los recursos IPC
make clean              # borra bin/ y obj/
make limpiar-ipc        # borra FIFO, memoria y semáforos huérfanos
make ayuda
```
 
**Versión FIFO** (dos terminales distintas):
 
```bash
./bin/servidor_fifo datos.csv     # terminal 1
./bin/cliente_fifo                # terminal 2
```
 
**Versión memoria compartida:**
 
```bash
./bin/servidor_shm datos.csv      # terminal 1
./bin/cliente_shm                 # terminal 2
```
 
**Menú del cliente:** 1 categoría · 2 rango de precio · 3 nombre · 4 cambiar límite · 5 salir y detener el servidor · 0 salir (el servidor sigue vivo).
 
El servidor también termina con `Ctrl+C` o `kill -TERM <pid>`, liberando sus recursos. Cliente y servidor son procesos independientes y no emparentados (sus padres son las shells de cada terminal).
 
---
 
## 4. Protocolo de comunicación
 
Todos los mensajes son estructuras binarias de tamaño fijo definidas en `comun/protocolo.h`. Cada estructura se verifica en compilación con `_Static_assert` para que quepa en `PIPE_BUF`.
 
### 4.1 Solicitud (`solicitud_t`, 160 bytes)
 
| Desplazamiento | Campo | Tipo | Significado |
|---|---|---|---|
| 0 | `magia` | `uint32_t` | `0x31435250` ("PRC1"), valida el mensaje |
| 4 | `operacion` | `int32_t` | código de operación |
| 8 | `pid_cliente` | `int32_t` | PID del cliente |
| 12 | `limite` | `int32_t` | máximo de resultados (0 = sin límite) |
| 16 | `precio_min` | `double` | rango inferior |
| 24 | `precio_max` | `double` | rango superior |
| 32 | `texto` | `char[128]` | categoría o parte del nombre |
 
| Código | Operación | Efecto |
|---|---|---|
| 1 | `OP_BUSCAR_CATEGORIA` | busca por categoría |
| 2 | `OP_BUSCAR_PRECIO` | busca por rango de precio |
| 3 | `OP_BUSCAR_NOMBRE` | busca por subcadena del nombre |
| 8 | `OP_DESCONECTAR` | el cliente termina; el servidor sigue vivo |
| 9 | `OP_TERMINAR` | orden de terminación del servidor |
 
### 4.2 Versión FIFO
 
1. El cliente envía la solicitud con **una sola** `write()` de 160 bytes por `FIFO_SOLICITUD`.
2. El servidor responde por `FIFO_RESPUESTA` con mensajes **cabecera + carga**, cada uno en una sola `write()`:
| Cabecera (8 bytes) | Carga |
|---|---|
| `tipo` (`int32`) + `longitud` (`int32`) | `longitud` bytes (máximo 512) |
 
| Tipo | Valor | Carga |
|---|---|---|
| `RESP_REGISTRO` | 1 | `registro_t` (128 bytes), un resultado |
| `RESP_FIN` | 2 | `resumen_t` (32 bytes), cierra la respuesta |
| `RESP_ERROR` | 3 | texto terminado en `\0` |
| `RESP_ADIOS` | 4 | sin carga; confirma `OP_DESCONECTAR` u `OP_TERMINAR` |
 
Una respuesta normal es `RESP_REGISTRO` × N seguido de `RESP_FIN`. Como cada mensaje es menor que `PIPE_BUF` (4096 en Linux), su escritura es atómica y no se mezcla con otra. Además `write_all`/`read_all` repiten la llamada si fue parcial.
 
### 4.3 Versión memoria compartida
 
La región `region_compartida_t` contiene: estado del servidor (`magia`, `pid_servidor`, `servidor_activo`), `estado`, `num_registros`, `numero_bloque`, la `solicitud`, el `resumen`, `mensaje_error` y un bloque de `registros[64]`.
 
| Semáforo | Valor inicial | Espera | Avisa | Significado |
|---|---|---|---|---|
| `sem_turno` | 1 | cliente | servidor (al terminar la consulta) | exclusión mutua entre clientes |
| `sem_solicitud` | 0 | servidor | cliente | hay una solicitud escrita |
| `sem_respuesta` | 0 | cliente | servidor | hay un bloque de respuesta listo |
| `sem_consumido` | 0 | servidor | cliente | el cliente ya copió el bloque |
 
```text
CLIENTE                                SERVIDOR
sem_wait(turno)                        sem_wait(solicitud)  [dormido]
escribe la solicitud
sem_post(solicitud) ─────────────────► copia la solicitud, recorre el CSV
sem_wait(respuesta) ◄───────────────── llena un bloque (≤ 64), sem_post(respuesta)
copia el bloque
sem_post(consumido) ─────────────────► sem_wait(consumido), sigue con el próximo bloque
        ... se repite por cada bloque ...
sem_wait(respuesta) ◄───────────────── último bloque (EST_FIN, EST_ERROR o EST_ADIOS)
sem_post(consumido) ─────────────────► estado = EST_LIBRE, sem_post(turno)
```
 
Estados de la región: `EST_LIBRE`, `EST_SOLICITUD`, `EST_BLOQUE`, `EST_FIN`, `EST_ERROR`, `EST_ADIOS`.
 
**Cómo se evita la condición de carrera:** solo el cliente que tiene el turno escribe la solicitud; el servidor solo reescribe el bloque después de que el cliente lo consumió (`sem_consumido`); y el cliente solo lee después de `sem_respuesta`, es decir, cuando el servidor terminó de escribir. Todas las esperas son `sem_timedwait` (el proceso duerme, no hay espera activa).
 
---
 
## 5. Recursos IPC utilizados
 
| Recurso | Tipo | Nombre | Lo crea | Lo elimina |
|---|---|---|---|---|
| FIFO de solicitudes | tubería nombrada | `/tmp/fifo_req_<GRUPO>` | servidor (`mkfifo`) | servidor (`unlink`) |
| FIFO de respuestas | tubería nombrada | `/tmp/fifo_resp_<GRUPO>` | servidor (`mkfifo`) | servidor (`unlink`) |
| Cerrojo de clientes | archivo + `fcntl` | `/tmp/fifo_lock_<GRUPO>` | cliente (`open` con `O_CREAT`) | servidor (`unlink`) |
| Región compartida | POSIX shm | `/dev/shm/shm_<GRUPO>` | servidor (`shm_open`) | servidor (`shm_unlink`) |
| Semáforos (4) | POSIX con nombre | `/dev/shm/sem.sem_<nombre>_<GRUPO>` | servidor (`sem_open`) | servidor (`sem_unlink`) |
 
**Descriptores abiertos durante la comunicación (FIFO):**
 
- `servidor_fifo`: 0, 1, 2; FIFO de solicitudes (solo lectura); FIFO de respuestas (solo escritura); un `FILE*` temporal sobre el CSV durante cada búsqueda.
- `cliente_fifo`: 0, 1, 2; archivo del cerrojo; FIFO de solicitudes (solo escritura); FIFO de respuestas (solo lectura).
---
 
## 6. Manejo de errores
 
| Situación | Comportamiento |
|---|---|
| El CSV no existe o no se puede leer | El servidor lo comprueba con `access()` **antes** de crear recursos IPC y termina con error |
| Cliente FIFO: la FIFO no existe (`ENOENT`) | Mensaje "¿Está en ejecución servidor_fifo?" y termina |
| Cliente FIFO: la FIFO existe pero nadie la lee (`ENXIO`) | Reintenta cada segundo hasta 5 s y luego informa que el servidor no está |
| Dos clientes FIFO a la vez | El segundo espera su turno con `fcntl(F_SETLKW)`; el núcleo libera el cerrojo si el primero muere |
| Cliente FIFO muere o cierra | El servidor ve `read()` = 0 (EOF) y espera a otro cliente |
| Servidor FIFO muere | El cliente ve EOF al leer o `EPIPE` al escribir (`SIGPIPE` está ignorada) |
| Mensaje truncado, `magia` o longitud inválidas | Se informa el error de protocolo y se cierra la sesión |
| Solicitud con parámetros inválidos | El servidor responde `RESP_ERROR` / `EST_ERROR` con el motivo |
| Línea del CSV con formato incorrecto | Se cuenta en `lineas_invalidas` y se ignora |
| SHM: ya hay un servidor vivo | El segundo servidor no arranca e informa el PID del primero |
| SHM: quedaron recursos de una ejecución incorrecta | El servidor los elimina al arrancar si su PID ya no existe; el cliente avisa y sugiere `make limpiar-ipc` |
| SHM: el cliente muere en plena consulta | El servidor lo detecta con `kill(pid, 0)` (cada 2 s), descarta avisos pendientes y devuelve el turno |
| SHM: el servidor muere | El cliente lo detecta en su espera vigilada y termina con un mensaje de error |
| `SIGINT` / `SIGTERM` | Una bandera `volatile sig_atomic_t` provoca el cierre ordenado y la liberación de los recursos |
| Retornos de llamadas al sistema | Todos se comprueban (`open`, `read`, `write`, `close`, `mmap`, `sem_*`, `malloc`, etc.) |
 
---
 
## 7. Observaciones con herramientas de Linux
 
### Observación 1: llamadas al sistema de la comunicación FIFO (`strace`)
 
```bash
strace -f -e trace=openat,read,write,close,unlink ./bin/servidor_fifo datos.csv
```
 
Salida esperada:
```bash
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
close(3)                                = 0
openat(AT_FDCWD, "/usr/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
read(3, "\177ELF\2\1\1\3\0\0\0\0\0\0\0\0\3\0>\0\1\0\0\0 \250\2\0\0\0\0\0"..., 832) = 832
close(3)                                = 0
write(1, "[servidor_fifo] FIFO creada: /tm"..., 58[servidor_fifo] FIFO creada: /tmp/fifo_req_Los_illuminati
) = 58
write(1, "[servidor_fifo] FIFO creada: /tm"..., 59[servidor_fifo] FIFO creada: /tmp/fifo_resp_Los_illuminati
) = 59
write(1, "[servidor_fifo] PID=1839 PPID=18"..., 56[servidor_fifo] PID=1839 PPID=1837 | dataset: datos.csv
) = 56
write(1, "[servidor_fifo] Ctrl+C o 'kill -"..., 72[servidor_fifo] Ctrl+C o 'kill -TERM 1839' para terminar ordenadamente.
) = 72
write(1, "[servidor_fifo] Esperando client"..., 87[servidor_fifo] Esperando cliente (open bloqueante de /tmp/fifo_req_Los_illuminati)...
) = 87
openat(AT_FDCWD, "/tmp/fifo_req_Los_illuminati", O_RDONLY
```
Cabe recordar que no tiene que ser los mismos datos ya que cada terminal y proceso tiene su propio PID, pero si debe seguir la misma estructura.

Qué verificar: el `openat(..., O_RDONLY)` de la FIFO de solicitudes no retorna hasta que el cliente se conecta; la solicitud llega en un único `read(..., 160)`; las respuestas salen en `write()` de 136 bytes (8 de cabecera + 128 de `registro_t`) y de 40 (8 + 32 de `resumen_t`); al final aparecen los `close` y los `unlink`. 
 
### Observación 2: proceso bloqueado esperando IPC (`ps`, `/proc`)
 
```bash
ps -o pid,ppid,stat,wchan:25,cmd -p $(pgrep servidor_shm)
grep -E 'State|VmRSS' /proc/$(pgrep servidor_shm)/status
```

Salida esperada:
```bash
    PID    PPID STAT WCHAN                     CMD
   2308    1121 S+   anon_pipe_read            ./bin/servidor_fifo

State:  S (sleeping)
VmRSS:      1892 kB
```
 
Qué verificar: estado `S` (sueño interrumpible) y un `WCHAN` relacionado con `futex` (el nombre exacto depende del kernel). Esto demuestra que el servidor espera dormido y no consume CPU. Repetir con `servidor_fifo` esperando cliente y comparar. De esta manera se observan los estados de los procesos y tambien muestra en que punto del kernel esta esperando cada proceso gracias al `WCHAN`.
 
### Observación 3: descriptores y recursos IPC (`lsof`, `ls`)
 
```bash
lsof -p $(pgrep servidor_fifo)
ls -l /tmp/fifo_*_Los_illuminati
lsof -p $(pgrep servidor_shm)
ls -l /dev/shm/
```
 
Salida esperada:
```bash
COMMAND    PID  USER  FD   TYPE DEVICE SIZE/OFF  NODE NAME
servidor_ 2308 josue cwd    DIR   8,48     4096  2707 /home/josue/Practica1
servidor_ 2308 josue rtd    DIR   8,48     4096     2 /
servidor_ 2308 josue txt    REG   8,48    50152 42089 /home/josue/Practica1/bin/servidor_fifo
servidor_ 2308 josue mem    REG   8,48  2190608 13605 /usr/lib/x86_64-linux-gnu/libc.so.6
servidor_ 2308 josue mem    REG   8,48   254864 13602 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
servidor_ 2308 josue   0u   CHR  136,3      0t0     6 /dev/pts/3
servidor_ 2308 josue   1u   CHR  136,3      0t0     6 /dev/pts/3
servidor_ 2308 josue   2u   CHR  136,3      0t0     6 /dev/pts/3
servidor_ 2308 josue   3r  FIFO   0,72      0t0   113 /tmp/fifo_req_Los_illuminati (deleted)
servidor_ 2308 josue   4w  FIFO   0,72      0t0   114 /tmp/fifo_resp_Los_illuminati (deleted)
-rw------- 1 josue josue 0 Oct 14 23:26 /tmp/fifo_lock_Los_illuminati
COMMAND    PID  USER  FD   TYPE DEVICE SIZE/OFF  NODE NAME
servidor_ 2702 josue cwd    DIR   8,48     4096  2707 /home/josue/Practica1
servidor_ 2702 josue rtd    DIR   8,48     4096     2 /
servidor_ 2702 josue txt    REG   8,48    57832 44922 /home/josue/Practica1/bin/servidor_shm
servidor_ 2702 josue mem    REG   8,48  2190608 13605 /usr/lib/x86_64-linux-gnu/libc.so.6
servidor_ 2702 josue DEL    REG   0,65             31 /dev/shm/sem.2Mcymj
servidor_ 2702 josue DEL    REG   0,65             30 /dev/shm/sem.bbGQxl
servidor_ 2702 josue DEL    REG   0,65             29 /dev/shm/sem.LH6W0V
servidor_ 2702 josue DEL    REG   0,65             28 /dev/shm/sem.PHhJrU
servidor_ 2702 josue mem    REG   0,65     8664    27 /dev/shm/shm_Los_illuminati
servidor_ 2702 josue mem    REG   8,48   254864 13602 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
servidor_ 2702 josue   0u   CHR  136,5      0t0     8 /dev/pts/5
servidor_ 2702 josue   1u   CHR  136,5      0t0     8 /dev/pts/5
servidor_ 2702 josue   2u   CHR  136,5      0t0     8 /dev/pts/5
servidor_ 2702 josue   3u   REG   0,65     8664    27 /dev/shm/shm_Los_illuminati
total 28
-rw------- 1 josue josue   32 Oct 14 23:39 sem.sem_consumido_Los_illuminati
-rw------- 1 josue josue   32 Oct 14 23:39 sem.sem_respuesta_Los_illuminati
-rw------- 1 josue josue   32 Oct 14 23:39 sem.sem_solicitud_Los_illuminati
-rw------- 1 josue josue   32 Oct 14 23:39 sem.sem_turno_Los_illuminati
-rw------- 1 josue josue 8664 Oct 14 23:39 shm_Los_illuminati
```
 
Nota: `ipcs` **no** lista memoria compartida ni semáforos POSIX (solo los de System V). Estos recursos se ven en `/dev/shm`. Tamaño de la región (`sizeof(region_compartida_t)`) en nuestra máquina: 8664 bytes*.
 
### Observación 4 (opcional): relación entre procesos
 
```bash
pstree -p -s "$(pgrep -x servidor_fifo)"
pstree -p -s "$(pgrep -x cliente_fifo)"
```
Salida esperada:
```bash
systemd(1)───init-systemd(Ub(2)───SessionLeader(1115)───Relay(1121)(1117)───bash(1121)───servidor_fifo(2308)
systemd(1)───init-systemd(Ub(2)───SessionLeader(809)───Relay(811)(810)───bash(811)───cliente_fifo(2314)
```
Estos comandos permiten analizar la jerarqu[ia de procesos del sistema y, específicamente, qué procesos dieron origen al servidor FIFO. Es útil porque permite relacionar los conceptos de PID, PPID, proceso padre y proceso hijo.
 
---
 
## 8. Preguntas del enunciado
 
### Parte A (FIFO)
 
1. **¿Qué ocurre si el cliente abre una FIFO antes de que exista el servidor?** Si el archivo no existe, `open()` falla con `ENOENT`. Si existe pero nadie la lee, un `open(O_WRONLY)` normal se bloquea hasta que aparezca un lector; con `O_NONBLOCK` falla con `ENXIO`. Nuestro cliente usa `O_NONBLOCK`, reintenta durante 5 s y luego informa el error.
2. **¿Cuándo se bloquea `read()` o `write()`?** `read()` se bloquea cuando la tubería está vacía y algún proceso la mantiene abierta en escritura (si no hay escritores devuelve 0). `write()` se bloquea cuando el búfer del núcleo está lleno (64 KB por defecto) y el lector no consume.
3. **¿Qué descriptores mantiene cada proceso?** Ver la sección 5.
4. **¿Qué recursos del SO intervienen?** El inodo de la FIFO en `/tmp` (solo punto de encuentro, sin datos en disco), el búfer circular de la tubería en el núcleo, las colas de espera del planificador y la tabla de descriptores de cada proceso.
### Parte B (memoria compartida)
 
1. **¿Por qué requiere sincronización explícita?** Porque `mmap()` solo mapea las mismas páginas físicas en ambos procesos; después los accesos son instrucciones normales de la CPU y el núcleo no interviene ni bloquea a nadie.
2. **¿Qué condición de carrera aparecería sin ella?** El cliente podría leer un registro a medio escribir, el servidor podría sobrescribir el bloque antes de que el cliente lo copie, o dos clientes podrían mezclar bytes de sus solicitudes.
3. **FIFO vs. memoria compartida:** la FIFO copia los datos usuario → núcleo → usuario y se sincroniza sola; la memoria compartida evita esas copias pero exige semáforos explícitos.
4. **¿Qué pasa con los recursos IPC si un proceso muere?** Los objetos POSIX (`/dev/shm`) persisten hasta `shm_unlink`/`sem_unlink` o hasta reiniciar; un semáforo tomado por un proceso muerto queda bloqueado. Por eso el servidor limpia los restos al arrancar y las esperas vigilan al otro proceso. En FIFO, el núcleo cierra los descriptores del proceso muerto y el otro extremo ve EOF o `EPIPE`, pero el archivo de la FIFO queda en `/tmp`.
### Preguntas de sustentación (practicar con el código)
 
- Identificar procesos con sus PID y PPID (`ps`, `pstree`, `/proc/<pid>/status`).
- Explicar una llamada al sistema vista con `strace` (por ejemplo `ftruncate` tras `shm_open`: el objeto nace con 0 bytes y sin `ftruncate` el acceso produce `SIGBUS`).
- Mostrar cuándo y por qué un proceso queda bloqueado (Observación 2).
- Terminar un proceso con `kill -9` y explicar el efecto sobre el otro (secciones 6 y 8).
- Identificar y liberar un recurso huérfano: `ls /tmp/fifo_* /dev/shm`, `lsof`, y `make limpiar-ipc`.
---
 
## 9. Limitaciones conocidas
 
- Cada servidor atiende un cliente a la vez; los demás esperan su turno (cerrojo en FIFO, `sem_turno` en SHM).
- El lector de CSV no admite comillas ni comas dentro de los campos.
- En SHM, si un cliente muere con `SIGKILL` justo después de tomar el turno y antes de avisar su solicitud, el turno no se devuelve.
- Los mensajes son binarios y asumen cliente y servidor compilados en la misma máquina y con el mismo compilador.
---
 
## 10. Uso de herramientas de IA
 
Declaración breve del uso significativo de herramientas de IA:
 
- **Herramienta(s) utilizada(s):** Claude, ChatGPT, Copilot de Vscode
- **Para qué se usaron:** Consultar conceptos de FIFO y semáforos POSIX, depurar errores, revisar el diseño del protocolo, ayudar con la documentación y auto completación de codigo en ciertas situaciones.
- **Qué hizo el grupo por su cuenta:** Implementación de la estructura del codigo, diseño e infraestructura de la comunicación entre archivos y pruebas del codigo en entornos funcionales.
Cada integrante comprende y puede modificar todo el código entregado.
 
---
 
## 11. Control de versiones
 
Repositorio: https://github.com/crdiazo/Procesos-llamadas-al-sistema-e-IPC-en-Linux. El historial refleja el trabajo de cada integrante (`git log --oneline --author="nombre"`). 
