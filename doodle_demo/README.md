# Doodle → Emoji demo

This isolated demo runs the pretrained 345-class
[`quickdraw-345-tflite`](https://huggingface.co/1starun8-research/quickdraw-345-tflite)
SE-ResNet on the Mac. Draw in the browser and pause briefly (or press
**Predict**) to see the strongest emoji-backed QuickDraw matches.

Nothing outside `doodle_demo/` is required or modified.

## Run

```bash
cd doodle_demo
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python app.py
```

Open <http://127.0.0.1:8766> if the browser does not open automatically. The
first prediction downloads roughly 8.4 MB of model weights; later runs use the
Hugging Face cache.

The former MobileViT model remains available for comparison:

```bash
.venv/bin/python app.py --backend mobilevit
```

Run the lightweight server tests without installing the ML dependencies:

```bash
python3 -m unittest doodle_demo.test_app -v
```

## ESP32 image endpoint

Bind to the LAN when the ESP32 is ready to send images:

```bash
.venv/bin/python app.py --host 0.0.0.0
```

Send an opaque PNG or JPEG containing only the drawing canvas:

```bash
curl --data-binary @drawing.png \
  -H 'Content-Type: image/png' \
  'http://127.0.0.1:8766/api/predict?top_k=5'
```

The response is stable JSON:

```json
{
  "model": "1starun8-research/quickdraw-345-tflite",
  "predictions": [
    {"label": "cat", "score": 0.82, "emoji": "🐱"}
  ]
}
```
