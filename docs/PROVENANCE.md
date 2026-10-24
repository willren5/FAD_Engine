# Code and design provenance

## Project scope

This repository contains the independent Engine extracted from the FAD synthesis
implementation, together with newly prepared examples, documentation and skills.
The identified project company is [FAD Records](../legal/PROJECT-LICENSOR.md).
Existing notices are retained with the company/project attribution supplied by
the rights holder. Private products, assets, account code and original repository
history are not bundled.

The source package has one implementation. Host types, metadata serialization,
file jobs and product UI stay outside the Core. Core independence is tested by
actual build inputs, runtime dependencies and local execution, not just a search
for a vendor namespace.

## Mathematical and behavioral work

The extraction used private fixed-version numerical/behavioral comparisons for
smoothers, filters, delays, oversampling and other primitives. Independently
written mathematical/runtime code and matching numbers are technical evidence;
they do not by themselves establish authorship, ownership or permission.

`include/fad/engine/filters/PreparedAllpass2x.h` contains numerical design outputs
prepared with a JUCE9.0.3 FilterDesign reference tool. No JUCE source/runtime is
included. This numerical-data origin is specifically retained for rights review;
the absence of a runtime dependency is not a claim of copyright clearance.

Several compatibility modules use measured instrument behavior, phase/random
streams, waveform/range mappings and calibration. Module names identify those
semantics and do not grant third-party source, binary, preset, sample, patent or
trademark rights. No commercial reference-product assets are distributed here.

## Responsibilities before publication or adoption

The project rights holder must verify ownership/assignment and any applicable
prior license for the material it distributes. Company registration alone does
not prove a code assignment. Technical parity or successful tests do not replace
that review.

For third-party contributions, retain valid [CLA](../legal/CONTRIBUTOR-LICENSE-AGREEMENT.md)
or [improvement-grant](../legal/IMPROVEMENT-GRANT-NOTICE.md) records and separately
review third-party material before commercial adoption. Those grants do not
retroactively obtain older or unrelated work.

The optional SDK is independently licensed and pinned, not vendored. See
[third-party notices](../THIRD_PARTY_NOTICES.md). This document records origin
and scope; it is not a legal certification.
