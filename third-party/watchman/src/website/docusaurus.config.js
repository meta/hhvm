/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

const lightCodeTheme = require('prism-react-renderer/themes/github');
const darkCodeTheme = require('prism-react-renderer/themes/dracula');
const {fbContent} = require('docusaurus-plugin-internaldocs-fb/internal');

const CATEGORY_ORDER = [
  'Installation',
  'Invocation',
  'Compatibility',
  'Commands',
  'Queries',
  'Expression Terms',
  'Internals',
  'Troubleshooting',
];

// With JSDoc @type annotations, IDEs can provide config autocompletion
/** @type {import('@docusaurus/types').DocusaurusConfig} */
module.exports = {
  title: 'Watchman',
  tagline: 'A file watching service',
  url: 'https://meta.github.io',
  baseUrl: '/watchman/',
  onBrokenLinks: 'throw',
  onBrokenMarkdownLinks: 'throw',
  trailingSlash: false,
  favicon: 'img/favicon.png',
  organizationName: 'meta',
  projectName: 'watchman',

  presets: [
    [
      'docusaurus-plugin-internaldocs-fb/docusaurus-preset',
      /** @type {import('docusaurus-plugin-internaldocs-fb').PresetOptions} */
      {
        docs: {
          sidebarPath: require.resolve('./sidebars.js'),
          editUrl: fbContent({
            internal:
              'https://www.internalfb.com/code/fbsource/fbcode/watchman/oss/website',
            external: 'https://github.com/meta/watchman/tree/main/website',
          }),
          // Add support for category
          async sidebarItemsGenerator({
            defaultSidebarItemsGenerator,
            docs,
            ...args
          }) {
            const generatedItems = await defaultSidebarItemsGenerator({
              docs,
              ...args,
            });
            const postsById = Object.fromEntries(
              docs.map((doc) => [doc.id, doc]),
            );
            return (
              generatedItems
                // grouping posts by `.frontMatter.category` if it is specified
                .reduce((acc, item) => {
                  if (item.type === 'category' || !(item.id in postsById)) {
                    acc.push(item);
                    return acc;
                  }
                  const post = postsById[item.id];
                  if (!('category' in post.frontMatter)) {
                    acc.push(item);
                    return acc;
                  }
                  const category = post.frontMatter.category;
                  for (const exist of acc) {
                    if (exist.type === 'category' && exist.label === category) {
                      exist.items.push(item);
                      return acc;
                    }
                  }
                  acc.push({
                    type: 'category',
                    label: category,
                    items: [item],
                  });
                  return acc;
                }, [])
                // Sort the category order if they are specified in `CATEGORY_ORDER` above
                .sort((a, b) => {
                  if (a.type !== 'category' && b.type !== 'category') {
                    return 0;
                  }
                  if (a.type !== 'category') {
                    return 1;
                  }
                  if (b.type !== 'category') {
                    return -1;
                  }

                  return (
                    CATEGORY_ORDER.indexOf(a.label) -
                    CATEGORY_ORDER.indexOf(b.label)
                  );
                })
                // Sort items in the category if they have `sidebar_position` defined
                .map((item) => {
                  if (item.type !== 'category') {
                    return item;
                  }
                  item.items.sort((a, b) => {
                    const post_a = postsById[a.id];
                    if (post_a === undefined) {
                      return 1;
                    }
                    const post_b = postsById[b.id];
                    if (post_b === undefined) {
                      return -1;
                    }
                    // We assign 10000 to posts have no `sidebar_position`
                    // defined since we want them to be sorted after the posts
                    // having position defined.
                    return (
                      (post_a.frontMatter['sidebar_position'] ?? 10000) -
                      (post_b.frontMatter['sidebar_position'] ?? 10000)
                    );
                  });
                  return item;
                })
            );
          },
        },
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
        staticDocsProject: 'watchman',
      },
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    {
      image: 'img/watchman-social-card.png',
      navbar: {
        title: 'Watchman',
        logo: {
          alt: 'Watchman Logo',
          src: 'img/logo.png',
        },
        items: [
          {
            to: 'docs/install',
            position: 'left',
            label: 'Documentation',
            activeBaseRegex: 'watchman/docs/install',
          },
          {
            to: 'support',
            position: 'left',
            label: 'Support',
            activeBaseRegex: 'watchman/support',
          },
          {
            href: 'https://github.com/meta/watchman',
            position: 'right',
            className: 'header-github-link',
            'aria-label': 'GitHub repository',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Links',
            items: [
              {
                label: 'Documentation',
                to: 'docs/install',
              },
              {
                label: 'GitHub',
                to: 'https://github.com/meta/watchman',
              },
              {
                label: 'Getting Support',
                to: 'docs/troubleshooting',
              },
            ],
          },
          {
            title: 'Related Projects',
            items: [
              {
                label: 'Sapling',
                href: 'https://sapling-scm.com/',
              },
            ],
          },
          {
            title: 'Legal',
            // Please do not remove the privacy and terms, it's a legal requirement.
            items: [
              {
                label: 'Privacy',
                href: 'https://opensource.fb.com/legal/privacy/',
              },
              {
                label: 'Terms',
                href: 'https://opensource.fb.com/legal/terms/',
              },
              {
                label: 'Data Policy',
                href: 'https://opensource.fb.com/legal/data-policy/',
              },
              {
                label: 'Cookie Policy',
                href: 'https://opensource.fb.com/legal/cookie-policy/',
              },
            ],
          },
        ],
        logo: {
          alt: 'Meta Open Source Logo',
          // This default includes a positive & negative version, allowing for
          // appropriate use depending on your site's style.
          src: '/img/meta_opensource_logo_negative.svg',
          href: 'https://opensource.fb.com',
        },
        copyright: `Copyright © ${new Date().getFullYear()} Meta Platforms, Inc. Built with Docusaurus.`,
      },
      prism: {
        theme: lightCodeTheme,
        darkTheme: darkCodeTheme,
      },
      algolia: {
        appId: '6E2EXVACEQ',
        apiKey: '739597f672974b121d44f4c6aebe13d3',
        indexName: 'watchman',
      },
    },

  customFields: {
    fbRepoName: 'fbsource',
    ossRepoPath: 'fbcode/watchman',
  },
};
