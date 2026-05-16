// Vitest config — renderer-side tests live under tests/renderer/.
// Pure JS runs in Node; nothing here touches the DOM yet, so we don't
// need jsdom. If we later add component tests we can flip `environment`
// to 'jsdom' and add `@testing-library/react`.
export default {
  test: {
    include: ['tests/renderer/**/*.test.js', 'tests/integration/**/*.test.js'],
    environment: 'node',
  },
};
