# Contributing

Start with a concrete problem, reproducible inputs and the smallest relevant
module. Read [architecture](docs/ARCHITECTURE.md), the [developer guide](docs/DEVELOPMENT_GUIDE.zh-CN.md)
and the [development skills](skills/README.md).

Preserve parameter/state semantics, one resource/voice owner, complete publication,
bounded audio work and existing protection. Add or update focused regression
coverage when it demonstrates the change; avoid a new test framework for a small
fix. Record source/build/binary identity and what was actually executed.

Run relevant native checks locally. There is no project compilation workflow on
GitHub Actions. Do not upload private product sources, credentials, account data,
commercial presets or unclear-rights samples as issue attachments or fixtures.

Report compatibility changes explicitly. A new default, enum ordinal, normalized
mapping, state schema or latency/tail behavior needs a deliberate migration
decision. Do not hide old failures by changing thresholds or normalizing audio.

Before a code contribution can be merged, the maintainer must obtain explicit
acceptance of [Contributor License Agreement1.1](legal/CONTRIBUTOR-LICENSE-AGREEMENT.md) from a person with
authority over it. Viewing the repository or opening a discussion is not such
acceptance. Contributors retain copyright; the grant supports both Community
and separately agreed commercial distribution.

Include the problem, changed behavior, relevant validation and remaining limits
in a pull request. Maintainers decide scope and acceptance; a contribution does
not imply a support or release commitment.

## Commercial adoption and external forks

The CLA explicitly permits the identified company to integrate an accepted
contribution into paid, closed-source FAD Synth and to commercially sublicense
it. Keep the exact acceptance/authority/scope record before merging/adoption;
see [the acceptance template](legal/CLA-ACCEPTANCE.md).

Distributed Engine modifications under openFAD 0.1 have a separate,
limited section7A grant and [grant notice](legal/IMPROVEMENT-GRANT-NOTICE.md).
Mere public source, a bare fork, PR or Commercial License purchase does not
automatically license an independent product or prior work to FAD. Check the
actual notice/license version, authorship and third-party restrictions.
