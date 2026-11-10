# Test-only modules (never packaged, never in bin/modules): they exist to prove fault handling, the RT guard and
# ABI rejection. Their packages land in bin/test-modules/<id>/.
dsptb_add_module(dsptb_test_faulty ID test.faulty TEST_MODULE
  MANIFEST faulty/module.json SOURCES faulty/faulty.cpp)
dsptb_add_module(dsptb_test_allocating ID test.allocating TEST_MODULE
  MANIFEST allocating/module.json SOURCES allocating/allocating.cpp)
dsptb_add_module(dsptb_test_probe ID test.probe TEST_MODULE
  MANIFEST probe/module.json SOURCES probe/probe.cpp)
dsptb_add_module(dsptb_test_abi_mismatch ID test.abimismatch TEST_MODULE
  MANIFEST abi_mismatch/module.json SOURCES abi_mismatch/abi_mismatch.cpp)
# A real PRIVATE_MODULE, so package_contents proves that private modules stay out of the packages.
dsptb_add_module(dsptb_test_private ID test.private PRIVATE_MODULE
  MANIFEST private/module.json SOURCES private/private.cpp)
# Manifests that need the GPU runtime (docs/GPU_RUNTIME.md section 9.1). They reuse a DLL that is never loaded:
# a package that is gated or incompatible is not loaded at all.
dsptb_add_module(dsptb_test_gpu_gated ID test.gpu-gated TEST_MODULE
  MANIFEST gpu_gated/module.json SOURCES abi_mismatch/abi_mismatch.cpp)
dsptb_add_module(dsptb_test_gpu_future_abi ID test.gpu-future-abi TEST_MODULE
  MANIFEST gpu_future_abi/module.json SOURCES abi_mismatch/abi_mismatch.cpp)
dsptb_add_module(dsptb_test_gpu_unknown_requires ID test.gpu-unknown-requires TEST_MODULE
  MANIFEST gpu_unknown_requires/module.json SOURCES abi_mismatch/abi_mismatch.cpp)
