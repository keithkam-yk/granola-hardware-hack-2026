import unittest

try:
    from .predictor import _rank_emoji_predictions
except ImportError:
    from predictor import _rank_emoji_predictions


class PredictionRankingTests(unittest.TestCase):
    def test_filters_non_emoji_labels_and_renormalizes(self) -> None:
        predictions = _rank_emoji_predictions(
            [("cat", 0.2), ("zigzag", 0.7), ("dog", 0.1)], top_k=5
        )

        self.assertEqual([item.label for item in predictions], ["cat", "dog"])
        self.assertAlmostEqual(predictions[0].score, 2 / 3)
        self.assertEqual(predictions[0].emoji, "🐱")

    def test_normalizes_underscored_labels(self) -> None:
        predictions = _rank_emoji_predictions([("soccer_ball", 1.0)], top_k=1)

        self.assertEqual(predictions[0].label, "soccer ball")
        self.assertEqual(predictions[0].emoji, "⚽")


if __name__ == "__main__":
    unittest.main()
