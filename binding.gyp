{
  "targets": [{
    "target_name": "posixmq",
    "sources": [
      "src/posixmq.cc",
    ],
    "defines": ["NAPI_VERSION=3"],
    "cflags": ["-O3"],
    "ldflags": [],
    "link_settings": {
        "libraries": ["-lrt"]
    }
  }]
}
