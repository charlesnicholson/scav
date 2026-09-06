One layout element per chart, each small enough that every route in it can be
read by eye and asserted by name. The corpus next door is the other half of the
question: it says whether a real diagram comes out well, and cannot say which
element was wrong when it does not. Every defect this directory holds a chart
for was found on a corpus chart first, where it took a bisection to name.

A chart here earns its place by isolating a *shape*, not by being small. Adding
one means adding the assertion that would have caught the defect, in
`src/layout/gauntlet_tests.cpp`, beside the properties every chart is held to.
That file names its charts in an array, and `functional_tests/test_gauntlet.py`
holds the array and this directory to the same list — it also puts every chart
through `fmt --check`, `check`, and `render` at both profiles.

Two scales, and a number from one never checks a claim from the other. The unit
suite above lays these out with **no space requests**; `tools/baseline.py
--gauntlet` renders them under real text and `tools/audit.py --gauntlet` counts
what is wrong with the picture that produces.
