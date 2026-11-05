# Dream sample: code for the Structure panel: classes, methods, a nested class and function,
# async functions and decorators. Click an entry there to jump to it; running it is safe.
# SPDX-License-Identifier: Apache-2.0
import asyncio
import functools


def logged(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        print(f"calling {func.__name__}")
        return func(*args, **kwargs)
    return wrapper


class Shape:
    def __init__(self, name):
        self.name = name

    def area(self):
        raise NotImplementedError

    @property
    def label(self):
        return f"{self.name} {self.area():.2f}"


class Square(Shape):
    def __init__(self, side):
        super().__init__("square")
        self.side = side

    def area(self):
        return self.side * self.side

    @staticmethod
    def unit():
        return Square(1.0)

    class Builder:
        def build(self, side):
            return Square(side)


@logged
def describe(shapes):
    def by_area(shape):
        return shape.area()
    return [shape.label for shape in sorted(shapes, key=by_area)]


async def fetch_sizes():
    await asyncio.sleep(0)
    return [2.5, 1.5]


async def main():
    sizes = await fetch_sizes()
    return describe([Square(size) for size in sizes] + [Square.Builder().build(0.5), Square.unit()])


print(asyncio.run(main()))
