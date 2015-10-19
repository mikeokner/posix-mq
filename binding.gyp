{
  "targets": [{
    "target_name": "posix-mq",
    "sources": [
      "src/posixmq.cc",
    ],
    "cflags": ["-O3"],
    "ldflags": ["-lrt"],
    "include_dirs": [
      "<!(node -e \"require('nan')\")"
    ]
  }]
}
