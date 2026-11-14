# ECSE3038 Test 2: the bathroom fan

An ESP32 (simulated in Wokwi) reads humidity from a DHT22 and posts it to a FastAPI API,
authenticated with a per-fan API key. The API stores fans, readings and settings in
MongoDB Atlas, and publishes each fan's settings over MQTT. The board drives a fan LED
from those settings and reports its state.

## Run the API

1. `cd api`
2. `python3 -m venv venv` then `source venv/bin/activate`
3. `python -m pip install -r requirements.txt`
4. Create `api/.env` (never committed) with:
   - `MONGODB_URI=<your Atlas connection string>`
   - `MQTT_NAME=<your broker name>`
5. `uvicorn app:app --host 127.0.0.1 --port 8000`, then open `/docs`

## Run the board

1. Open the `board` folder in VS Code with PlatformIO and the Wokwi extension.
2. Create `board/include/secrets.h` (never committed): `#define DEVICE_KEY ""`
3. Build, then start the Wokwi simulator and copy the MAC from the Serial Monitor.
4. `POST /fans` with `{"mac": "<MAC>", "room": "bathroom"}` and put the returned key in `secrets.h`.
5. Build and start again. Set the fan with `PUT /fans/{mac}/settings`.

