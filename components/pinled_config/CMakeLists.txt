# The half of the schema layer that needs nanopb.
#
# This component exists to make a rule structural instead of merely documented.
# pinled_schema is reachable from filament.h and profiler.h, so anything it
# registers must link in a build with no nanopb — and the projection from
# documents onto runtime records obviously cannot. Keeping the two in one
# component meant that constraint lived in a comment, and a comment is not a
# build failure.
#
# So: nanopb-free vocabulary and framing live in pinled_schema; the projection
# that turns a decoded document into a MachineConfig and a ChannelConfig[] lives
# here. The host build has had exactly this split since step 1
# (test/host/CMakeLists.txt), where it is the `pinled_config` library.

idf_component_register(
        SRCS "src/pinled_resolve.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "pinled_schema" "nanopb")
