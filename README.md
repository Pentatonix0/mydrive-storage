# HW4 Лебедев Андрей БПИ234

Многопользовательское файловое хранилище по TCP.

Стек: **C++20 + Boost.Asio**.

## Зависимости

- CMake 3.22+
- C++20 compiler
- Boost.Asio headers
- OpenSSL

## Сборка

```bash
cmake -S . -B build
cmake --build build -j 1
```

## Запуск сервера

При необходимости отредактировать `config/server.json`:

```json
{
    "listen_host": "0.0.0.0",
    "listen_port": 9000,
    "storage_root": "./server_storage",
    "threads": 4
}
```

Запуск:

```bash
./build/mydrive_server --config config/server.json
```

## Запуск клиента

При необходимости отредактировать `config/client.json`:

```json
{
    "client_id": "lebedev-local",
    "server_host": "127.0.0.1",
    "server_port": 9000,
    "directory": "./client_files",
    "max_connections": 8,
    "transfer_mode": "sendfile"
}
```

Интерактивный режим:

```bash
./build/mydrive_client --config config/client.json
```

Один раунд синхронизации и выход:

```bash
./build/mydrive_client --config config/client.json --mode buffered --sync-once
./build/mydrive_client --config config/client.json --mode sendfile --sync-once
```

## Команды клиента

```text
help                         показать справку
sync                         запустить синхронизацию
status                       показать настройки и последнюю статистику
files                        показать локальные файлы, размеры и SHA-256
mode buffered                обычная передача через буфер
mode sendfile                передача через sendfile/DMA zero-copy
connections N                число TCP-соединений, от 1 до 32
exit                         завершить клиент
```

Синхронизация односторонняя: **клиент -> сервер**.

## Генерация тестовых файлов

```bash
./generate_files.sh client_files
./generate_big_file.sh client_files
./generate_random.sh --n 10 --dir client_files
```

- `generate_files.sh` создает файлы 50, 150 и 200 MB.
- `generate_big_file.sh` создает файл 1500 MB.
- `generate_random.sh --n N` создает `N` файлов случайного размера от 105 до 205 MB.
