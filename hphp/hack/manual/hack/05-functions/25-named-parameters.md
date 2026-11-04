# Named Parameters

When defining a Hack function or method, you can mark a parameter as named, using the `named` keyword. Callers of the function then must include the name of the parameter. Named parameters can make your APIs safer and easier to read:

```hack
function send_message(
    string $body,
    named string $host,
    named ?int $thread_id = null,
    named bool $urgent = false,
): void {
    // ...
}

function example(): void {
    send_message("hello", host="a.fb.com");
    send_message("hello", host="b.fb.com", urgent=true);
    send_message("hello", host="c.fb.com", thread_id=123, urgent=true);
}
```

In the `send_message` example above, `$body` is a positional parameter, but `$host`, `$thread_id`
and `$urgent` must be passed as `name=value`, making intent
clearer. The parameters `$thread_id` and `$urgent` have default values, and are
therefore optional.

Named parameters can prevent bugs from arguments that are easy to mix up,
especially optional or "configuration-like" parameters such as booleans,
nullable values, and multiple parameters of the same type.

```hack
function schedule_retry(
    string $job,
    named int $delay_ms = 1000,
    named int $max_attempts = 3,
): void {
    // ...
}

function example(): void {
    schedule_retry("refresh-cache", delay_ms=5000, max_attempts=5);
}
```

In the `schedule_retry` example above, named parameters can help make
the call site clearer with labels when there are multiple adjacent parameters of
the same type: named parameters prevent mixing up `$delay_ms` and `$max_attempts`.

As a rule of thumb, keep obviously required inputs positional and make
easily-confused or optional parameters named.

## Named Parameters in Types

Named parameters are part of a function's API and therefore are part of the type. The type of the `schedule_retry` function above is `function(string $job, named optional int $delay_ms, named optional int $max_attempts): void`.

## Things To Keep In Mind

- Since named parameters are part of the type of a function, renaming a named parameter is a breaking change for callers, and the same names must be preserved in method overrides and function types.
- Required named parameters must be provided, and unknown named arguments
  are errors.
- Named parameters can have default values.
- Evaluation order is left-to-right based on the order of arguments at the call site.
- Default values for optional parameters should be free of side effects
  in order to avoid interactions between different call sites.

## Prefer Named Parameters Over the "Shape-Passing Pattern"

Older code sometimes simulates named arguments by passing a shape. It is
recommended to use named parameters when you are designing or updating an API –
they make call sites clearer, avoid unpacking boilerplate inside the callee, and
are more efficient than the shape-based pattern.

**GOOD** (named parameters):

```hack
function schedule_retry(
    string $job,
    named int $delay_ms = 1000,
    named int $max_attempts = 3,
): void {
    // ...
}
```

**BAD** (inefficient, high-boilerplate shape-passing pattern):

```hack
function schedule_retry(string $job, shape(?"delay_ms" => int, ?"max_attempts" => int) $options): void {
    if (!Shapes::keyExists($options, "delay_ms")) {
        $options["delay_ms"] = 1000;
    }
    if (!Shapes::keyExists($options, "max_attempts")) {
        $options["max_attempts"] = 3;
    }
    // ...
}
```

Shape-passing can still be convenient when the same or similar options must be passed to multiple functions, but generally named parameters are preferred. Please reach out to Hack team ([internal link](https://fburl.com/workplace/i1f5coqc)) and share examples if you find yourself reaching for the shape-passing pattern frequently, as it could help evolve the language+codebase.
