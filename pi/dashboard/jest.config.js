module.exports = {
	testEnvironment: 'jsdom',
	testMatch: ['**/*.test.js'],
	collectCoverageFrom: [
		'static/**/*.js',
		'!static/**/*.test.js',
		'!static/app.js',  // Main file has DOM deps, tested separately
	],
	coverageThreshold: {
		global: {
			branches: 40,
			functions: 40,
			lines: 40,
			statements: 40,
		},
	},
	setupFilesAfterEnv: [],
};
