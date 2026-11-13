import os
import json
import secrets
from datetime import datetime, timezone
from typing import Literal

from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Header, Query, Response
from pydantic import BaseModel, Field
from pymongo import MongoClient
import paho.mqtt.publish as mqtt_publish

load_dotenv()

client = MongoClient(os.getenv("MONGODB_URI"), tz_aware=True)
db = client["ecse3038_test2"]
fans = db["fans"]
readings = db["readings"]

MQTT_HOST = "135.232.203.144"
MQTT_PORT = 1883
MQTT_NAME = os.getenv("MQTT_NAME")

app = FastAPI()


class Fan(BaseModel):
    mac: str
    room: str


class Reading(BaseModel):
    mac: str
    humidity: float = Field(ge=0, le=100)


class Settings(BaseModel):
    mode: Literal["auto", "on", "off"]
    max_humidity: float = Field(ge=0, le=100)


def check_key(mac: str, key: str | None):
    """401 if the key is missing or was never issued, 403 if it is another fan's key."""
    if not key:
        raise HTTPException(status_code=401, detail="Missing X-API-Key")
    owner = fans.find_one({"key": key})
    if owner is None:
        raise HTTPException(status_code=401, detail="Unknown API key")
    if owner["mac"] != mac:
        raise HTTPException(status_code=403, detail="That key belongs to another fan")


# ---------- A1: fans ----------

@app.post("/fans", status_code=201)
def create_fan(fan: Fan):
    if fans.find_one({"mac": fan.mac}) is not None:
        raise HTTPException(status_code=409, detail="A fan with that MAC already exists")
    key = secrets.token_urlsafe(16)
    fans.insert_one({"mac": fan.mac, "room": fan.room, "key": key})
    return {"mac": fan.mac, "room": fan.room, "key": key}


@app.get("/fans")
def get_fans():
    return list(fans.find({}, {"_id": 0, "key": 0}))


@app.get("/fans/{mac}")
def get_fan(mac: str):
    fan = fans.find_one({"mac": mac}, {"_id": 0, "key": 0})
    if fan is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + mac)
    return fan


# ---------- A2: POST /readings ----------

@app.post("/readings", status_code=201)
def create_reading(reading: Reading, x_api_key: str | None = Header(default=None)):
    check_key(reading.mac, x_api_key)
    new_reading = reading.model_dump()
    new_reading["time"] = datetime.now(timezone.utc)
    readings.insert_one(new_reading)
    new_reading.pop("_id")
    return new_reading


# ---------- A3: GET /readings ----------

@app.get("/readings")
def get_readings(mac: str | None = None, limit: int = Query(default=10, ge=1, le=100)):
    query = {}
    if mac is not None:
        query["mac"] = mac
    return list(readings.find(query, {"_id": 0}).sort("time", -1).limit(limit))


# ---------- A4: PUT /fans/{mac}/settings ----------

@app.put("/fans/{mac}/settings")
def put_settings(mac: str, settings: Settings, response: Response):
    fan = fans.find_one({"mac": mac})
    if fan is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + mac)

    new_settings = settings.model_dump()   # {"mode": "auto", "max_humidity": 65.0}
    fans.update_one({"mac": mac}, {"$set": {"settings": new_settings}})
    response.status_code = 200 if "settings" in fan else 201

    topic = "ecse3038/" + MQTT_NAME + "/fan/settings"
    try:
        mqtt_publish.single(topic, json.dumps(new_settings), retain=True,
                            hostname=MQTT_HOST, port=MQTT_PORT)
    except Exception as e:
        print("MQTT publish failed:", e)
    return new_settings


# ---------- A5: DELETE /fans/{mac} ----------

@app.delete("/fans/{mac}", status_code=204)
def delete_fan(mac: str, x_api_key: str | None = Header(default=None)):
    if fans.find_one({"mac": mac}) is None:
        raise HTTPException(status_code=404, detail="No fan with MAC " + mac)
    check_key(mac, x_api_key)
    fans.delete_one({"mac": mac})
    return Response(status_code=204)