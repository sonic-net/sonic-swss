import importlib.util
import json
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("post_sticky_comment.py")
SPEC = importlib.util.spec_from_file_location("post_sticky_comment", MODULE_PATH)
sticky = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sticky)


class Response:
    def __enter__(self):
        return self

    def __exit__(self, *args):
        return None

    def read(self):
        return json.dumps([]).encode()


class StickyCommentTest(unittest.TestCase):
    @mock.patch.object(sticky.urllib.request, "urlopen", return_value=Response())
    def test_api_request_has_finite_timeout(self, urlopen):
        sticky.api_request("GET", "https://api.github.com/example", "token")

        self.assertEqual(urlopen.call_args.kwargs["timeout"], sticky.REQUEST_TIMEOUT_SECONDS)

    @mock.patch.object(sticky, "api_request")
    def test_lookup_paginates_and_migrates_action_owned_legacy_comment(self, api_request):
        marker = "<!-- sticky-gate-comment:gate -->"
        legacy_marker = "<!-- Sticky Pull Request Commentgate -->"
        first_page = [
            {"id": index, "user": {"login": "someone"}, "body": marker}
            for index in range(sticky.COMMENTS_PER_PAGE)
        ]
        legacy_comment = {
            "id": 1234,
            "user": {"login": sticky.ACTION_COMMENT_AUTHOR},
            "body": f"{legacy_marker}\nold report",
        }
        api_request.side_effect = [first_page, [legacy_comment]]

        comment_id = sticky.find_existing_comment(
            "https://api.github.com/repos/example/repo",
            9,
            "token",
            (marker, legacy_marker),
        )

        self.assertEqual(comment_id, 1234)
        self.assertIn("page=1", api_request.call_args_list[0].args[1])
        self.assertIn("page=2", api_request.call_args_list[1].args[1])

    @mock.patch.object(sticky, "api_request")
    def test_lookup_ignores_embedded_or_non_action_markers(self, api_request):
        marker = "<!-- sticky-gate-comment:gate -->"
        api_request.return_value = [
            {"id": 1, "user": {"login": sticky.ACTION_COMMENT_AUTHOR}, "body": f"quoted {marker}"},
            {"id": 2, "user": {"login": "someone"}, "body": marker},
        ]

        self.assertIsNone(
            sticky.find_existing_comment(
                "https://api.github.com/repos/example/repo",
                9,
                "token",
                (marker,),
            )
        )


if __name__ == "__main__":
    unittest.main()
