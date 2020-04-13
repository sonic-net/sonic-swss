# Docker Virtual Switch (DVS) Development Guide

## `dvslib` Design Principles (WIP)

### 1. If it can't be reused, don't put it in `dvslib`.

The idea of `dvslib` is to facilitate collaboration and code-reuse between test authors. Adding code that's too specific to any one particular test case clutters the library and makes it more difficult for contributors to find the tools they need to write their own tests.

**Example:**
```

```

### 2. Prefer high-level APIs.

We want it to be easy to write new tests, and we want it to be easy for future maintainers to *understand* those tests. (WIP)

#### "Mixed" APIs
In some cases, it may be difficult or impossible to provide one universal interface for all test authors. This is especially true for foundational services like database access, syslog parsing, and so on.

For such APIs we still recommend you provide a high-level API for the most common use cases. However, it may be appropriate to also expose the "building-block" APIs you used under the hood to help users build their own APIs for their specific use-cases. `dvs_database` is a good example of this pattern.

### 3. Prefer object-based APIs, and expose those APIs through the `dvs` fixture.

Because our tests are primarily based on checking device state, we have to manage a lot of state and state updates in our tests. This is much simpler to do, and much simpler for other contributors to understand, if we abstract this state using object APIs.

**Example:**
```

```

Exposing these APIs through the `dvs` fixture makes it easier to implement system-wide features like test pre/post-checks and garbage collection. Additionally, we want to support multiple virtual switches in the future, and associating each object with its parent device makes that effort simpler.
