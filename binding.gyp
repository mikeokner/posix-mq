{
  "targets": [{
    "target_name": "posixmq",
    "sources": [
      "src/posixmq.cc",
    ],
    "cflags": ["-O3"],
    "ldflags": [],
    "link_settings": {
        "libraries": ["-lrt"]
    },
    "include_dirs": [
      "<!(node -e \"require('nan')\")"
    ]
  }]
}
