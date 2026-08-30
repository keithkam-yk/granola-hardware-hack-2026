"""Lazy QuickDraw inference behind a small, stable interface."""

from __future__ import annotations

import io
import math
import statistics
import threading
import warnings
from dataclasses import dataclass
from typing import Any


DEFAULT_MODEL_ID = "1starun8-research/quickdraw-345-tflite"
MOBILEVIT_MODEL_ID = "Xenova/quickdraw-mobilevit-small"
LITERT_MODEL_FILENAME = "quickdraw_model.tflite"
# The weights use this case-sensitive order (the three "The ..." labels come
# first), even though categories.txt contains a different display order.
LITERT_LABELS_FILENAME = "labels.txt"

EMOJI_BY_LABEL = {
    "airplane": "✈️",
    "alarm clock": "⏰",
    "ambulance": "🚑",
    "angel": "😇",
    "ant": "🐜",
    "apple": "🍎",
    "banana": "🍌",
    "basketball": "🏀",
    "bat": "🦇",
    "bear": "🐻",
    "bee": "🐝",
    "bicycle": "🚲",
    "bird": "🐦",
    "birthday cake": "🎂",
    "book": "📖",
    "broccoli": "🥦",
    "bus": "🚌",
    "butterfly": "🦋",
    "cactus": "🌵",
    "cake": "🍰",
    "calendar": "📅",
    "camera": "📷",
    "candle": "🕯️",
    "car": "🚗",
    "carrot": "🥕",
    "cat": "🐱",
    "cell phone": "📱",
    "clock": "🕐",
    "cloud": "☁️",
    "coffee cup": "☕",
    "cookie": "🍪",
    "cow": "🐄",
    "crab": "🦀",
    "crocodile": "🐊",
    "crown": "👑",
    "dog": "🐶",
    "dolphin": "🐬",
    "donut": "🍩",
    "dragon": "🐉",
    "duck": "🦆",
    "elephant": "🐘",
    "envelope": "✉️",
    "eye": "👁️",
    "eyeglasses": "👓",
    "face": "🙂",
    "fish": "🐟",
    "flamingo": "🦩",
    "flower": "🌸",
    "football": "🏈",
    "frog": "🐸",
    "grapes": "🍇",
    "guitar": "🎸",
    "hamburger": "🍔",
    "hammer": "🔨",
    "hand": "✋",
    "headphones": "🎧",
    "helicopter": "🚁",
    "horse": "🐴",
    "hot dog": "🌭",
    "house": "🏠",
    "ice cream": "🍦",
    "key": "🔑",
    "knife": "🔪",
    "leaf": "🍃",
    "light bulb": "💡",
    "lightning": "⚡",
    "lion": "🦁",
    "lollipop": "🍭",
    "microphone": "🎤",
    "monkey": "🐒",
    "moon": "🌙",
    "mouse": "🐭",
    "mushroom": "🍄",
    "octopus": "🐙",
    "onion": "🧅",
    "owl": "🦉",
    "palm tree": "🌴",
    "panda": "🐼",
    "parachute": "🪂",
    "peanut": "🥜",
    "pear": "🍐",
    "penguin": "🐧",
    "piano": "🎹",
    "pig": "🐷",
    "pineapple": "🍍",
    "pizza": "🍕",
    "rabbit": "🐰",
    "radio": "📻",
    "rain": "🌧️",
    "rainbow": "🌈",
    "rhinoceros": "🦏",
    "sailboat": "⛵",
    "sandwich": "🥪",
    "school bus": "🚌",
    "scissors": "✂️",
    "shark": "🦈",
    "sheep": "🐑",
    "shoe": "👟",
    "skateboard": "🛹",
    "skull": "💀",
    "smiley face": "🙂",
    "snail": "🐌",
    "snake": "🐍",
    "snowflake": "❄️",
    "snowman": "☃️",
    "soccer ball": "⚽",
    "spider": "🕷️",
    "spoon": "🥄",
    "star": "⭐",
    "strawberry": "🍓",
    "sun": "☀️",
    "swan": "🦢",
    "sword": "⚔️",
    "syringe": "💉",
    "teddy-bear": "🧸",
    "telephone": "☎️",
    "television": "📺",
    "tent": "⛺",
    "tiger": "🐯",
    "toilet": "🚽",
    "tooth": "🦷",
    "toothbrush": "🪥",
    "tractor": "🚜",
    "traffic light": "🚦",
    "train": "🚆",
    "tree": "🌳",
    "t-shirt": "👕",
    "umbrella": "☂️",
    "violin": "🎻",
    "watermelon": "🍉",
    "whale": "🐋",
    "wine glass": "🍷",
    "zebra": "🦓",
}


class PredictionError(Exception):
    """Base class for errors safe to translate into an API response."""


class InvalidImage(PredictionError):
    """The submitted bytes are not a useful drawing."""


class ModelUnavailable(PredictionError):
    """The model or its optional runtime cannot be loaded."""


@dataclass(frozen=True)
class Prediction:
    label: str
    score: float
    emoji: str | None

    def as_dict(self) -> dict[str, str | float | None]:
        return {"label": self.label, "score": self.score, "emoji": self.emoji}


def _rank_emoji_predictions(
    label_scores: Any, *, top_k: int
) -> tuple[Prediction, ...]:
    """Rank and renormalize the model's scores over emoji-backed labels."""
    supported: list[tuple[str, float]] = []
    seen: set[str] = set()
    for raw_label, raw_score in label_scores:
        label = str(raw_label).strip().lower().replace("_", " ")
        score = float(raw_score)
        if (
            label in EMOJI_BY_LABEL
            and label not in seen
            and math.isfinite(score)
            and score >= 0
        ):
            supported.append((label, score))
            seen.add(label)

    supported.sort(key=lambda item: item[1], reverse=True)
    total = sum(score for _, score in supported)
    if not supported or total <= 0:
        raise PredictionError("The model returned no usable emoji predictions")
    return tuple(
        Prediction(label, score / total, EMOJI_BY_LABEL[label])
        for label, score in supported[:top_k]
    )


def _normalise_drawing(
    image_bytes: bytes,
    *,
    image_module: Any,
    image_chops: Any,
    image_ops: Any,
    white_background: bool,
) -> Any:
    try:
        with image_module.open(io.BytesIO(image_bytes)) as source:
            source.load()
            image = source.convert("L")
    except Exception as error:
        raise InvalidImage("The request body is not a readable PNG or JPEG") from error

    corners = (
        image.getpixel((0, 0)),
        image.getpixel((image.width - 1, 0)),
        image.getpixel((0, image.height - 1)),
        image.getpixel((image.width - 1, image.height - 1)),
    )
    background = round(statistics.median(corners))
    difference = image_chops.difference(
        image, image_module.new("L", image.size, background)
    ).point(lambda value: 255 if value > 12 else 0)
    bounds = difference.getbbox()
    if bounds is None:
        raise InvalidImage("Draw something before asking for a prediction")

    cropped = image.crop(bounds)
    target_background = 255 if white_background else 0
    if (background > 127) != white_background:
        cropped = image_ops.invert(cropped)

    margin = max(2, round(max(cropped.size) * 0.12))
    side = max(cropped.size) + margin * 2
    square = image_module.new("L", (side, side), target_background)
    offset = ((side - cropped.width) // 2, (side - cropped.height) // 2)
    square.paste(cropped, offset)
    return square


class LiteRTQuickDrawPredictor:
    """Run the compact SE-ResNet QuickDraw model through Google LiteRT."""

    def __init__(self, model_id: str = DEFAULT_MODEL_ID) -> None:
        self.model_id = model_id
        self._lock = threading.Lock()
        self._interpreter: Any | None = None
        self._labels: tuple[str, ...] = ()
        self._numpy: Any | None = None
        self._image_module: Any | None = None
        self._image_chops: Any | None = None
        self._image_ops: Any | None = None
        self._input_index: int | None = None
        self._output_index: int | None = None
        self._load_error: str | None = None

    @property
    def status(self) -> str:
        if self._interpreter is not None:
            return "ready"
        if self._load_error is not None:
            return "unavailable"
        return "not_loaded"

    def predict(self, image_bytes: bytes, *, top_k: int = 5) -> tuple[Prediction, ...]:
        if not 1 <= top_k <= 10:
            raise ValueError("top_k must be between 1 and 10")

        with self._lock:
            self._load()
            assert self._interpreter is not None
            assert self._numpy is not None
            assert self._image_module is not None
            assert self._input_index is not None
            assert self._output_index is not None

            # Despite the model card's white-background example, the released
            # weights use Google QuickDraw's native black canvas / white ink.
            image = _normalise_drawing(
                image_bytes,
                image_module=self._image_module,
                image_chops=self._image_chops,
                image_ops=self._image_ops,
                white_background=False,
            ).resize((28, 28), self._image_module.Resampling.LANCZOS)
            image_array = self._numpy.asarray(image, dtype=self._numpy.float32)
            image_array = (image_array / 255.0).reshape((1, 28, 28, 1))
            self._interpreter.set_tensor(self._input_index, image_array)
            self._interpreter.invoke()
            scores = self._interpreter.get_tensor(self._output_index).reshape(-1)
            if len(scores) != len(self._labels):
                raise PredictionError("The model output does not match its label list")
            return _rank_emoji_predictions(zip(self._labels, scores), top_k=top_k)

    def _load(self) -> None:
        if self._interpreter is not None:
            return
        if self._load_error is not None:
            raise ModelUnavailable(self._load_error)
        try:
            import numpy
            from ai_edge_litert.interpreter import Interpreter
            from huggingface_hub import hf_hub_download
            from PIL import Image, ImageChops, ImageOps

            def download(filename: str) -> str:
                try:
                    return hf_hub_download(
                        self.model_id, filename, local_files_only=True
                    )
                except Exception:
                    return hf_hub_download(self.model_id, filename)

            model_path = download(LITERT_MODEL_FILENAME)
            labels_path = download(LITERT_LABELS_FILENAME)
            interpreter = Interpreter(model_path=model_path, num_threads=4)
            interpreter.allocate_tensors()
            input_details = interpreter.get_input_details()
            output_details = interpreter.get_output_details()
            if len(input_details) != 1 or len(output_details) != 1:
                raise ValueError("expected one input and one output tensor")

            labels = tuple(
                line.strip().lower()
                for line in open(labels_path, encoding="utf-8")
                if line.strip()
            )
            self._interpreter = interpreter
            self._labels = labels
            self._input_index = int(input_details[0]["index"])
            self._output_index = int(output_details[0]["index"])
            self._numpy = numpy
            self._image_module = Image
            self._image_chops = ImageChops
            self._image_ops = ImageOps
        except ImportError as error:
            self._load_error = (
                "QuickDraw dependencies are missing. Install doodle_demo/requirements.txt."
            )
            raise ModelUnavailable(self._load_error) from error
        except Exception as error:
            self._load_error = f"Could not load {self.model_id}: {error}"
            raise ModelUnavailable(self._load_error) from error


class HuggingFaceQuickDrawPredictor:
    """Predict QuickDraw labels while hiding model and image-normalization details."""

    def __init__(self, model_id: str = MOBILEVIT_MODEL_ID) -> None:
        self.model_id = model_id
        self._lock = threading.Lock()
        self._processor: Any | None = None
        self._model: Any | None = None
        self._torch: Any | None = None
        self._numpy: Any | None = None
        self._image_module: Any | None = None
        self._image_chops: Any | None = None
        self._image_ops: Any | None = None
        self._load_error: str | None = None

    @property
    def status(self) -> str:
        if self._model is not None:
            return "ready"
        if self._load_error is not None:
            return "unavailable"
        return "not_loaded"

    def predict(self, image_bytes: bytes, *, top_k: int = 5) -> tuple[Prediction, ...]:
        if not 1 <= top_k <= 10:
            raise ValueError("top_k must be between 1 and 10")

        with self._lock:
            self._load()
            image = self._normalise_image(image_bytes)
            assert self._processor is not None
            assert self._model is not None
            assert self._torch is not None
            assert self._numpy is not None

            # MobileViT is configured for one channel, but its Python image
            # processor expects an explicit channel dimension rather than a
            # two-dimensional Pillow grayscale image.
            image_array = self._numpy.asarray(image, dtype=self._numpy.uint8)[..., None]
            inputs = self._processor(images=image_array, return_tensors="pt")
            with self._torch.inference_mode():
                logits = self._model(**inputs).logits[0]
                probabilities = self._torch.softmax(logits, dim=-1)
            return _rank_emoji_predictions(
                (
                    (self._model.config.id2label[index], score)
                    for index, score in enumerate(probabilities.tolist())
                ),
                top_k=top_k,
            )

    def _load(self) -> None:
        if self._model is not None:
            return
        if self._load_error is not None:
            raise ModelUnavailable(self._load_error)
        try:
            import torch
            import numpy
            from PIL import Image, ImageChops, ImageOps
            from transformers import AutoImageProcessor, AutoModelForImageClassification

            def load_model(*, local_files_only: bool) -> tuple[Any, Any]:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", FutureWarning)
                    processor = AutoImageProcessor.from_pretrained(
                        self.model_id,
                        use_fast=False,
                        local_files_only=local_files_only,
                    )
                model = AutoModelForImageClassification.from_pretrained(
                    self.model_id, local_files_only=local_files_only
                )
                return processor, model

            try:
                self._processor, self._model = load_model(local_files_only=True)
            except OSError:
                self._processor, self._model = load_model(local_files_only=False)
            self._model.eval()
            self._torch = torch
            self._numpy = numpy
            self._image_module = Image
            self._image_chops = ImageChops
            self._image_ops = ImageOps
        except ImportError as error:
            self._load_error = (
                "QuickDraw dependencies are missing. Install doodle_demo/requirements.txt."
            )
            raise ModelUnavailable(self._load_error) from error
        except Exception as error:
            self._load_error = f"Could not load {self.model_id}: {error}"
            raise ModelUnavailable(self._load_error) from error

    def _normalise_image(self, image_bytes: bytes) -> Any:
        assert self._image_module is not None
        assert self._image_chops is not None
        assert self._image_ops is not None
        return _normalise_drawing(
            image_bytes,
            image_module=self._image_module,
            image_chops=self._image_chops,
            image_ops=self._image_ops,
            white_background=False,
        )
