import {css} from "chrome://resources/lit/v3_0/lit.rollup.js";

export const verticalScrollbarStyles = css`
  .list {
    scrollbar-width: thin;
    scrollbar-color: var(--vertical-bar-scroll-thumb, rgba(95, 99, 104, 0.3)) transparent;
  }

  .list::-webkit-scrollbar {
    inline-size: 8px;
  }

  .list::-webkit-scrollbar-track {
    background: transparent;
  }

  .list::-webkit-scrollbar-thumb {
    border-radius: 8px;
    background-color: var(--vertical-bar-scroll-thumb, rgba(95, 99, 104, 0.3));
    border: 2px solid transparent;
    background-clip: padding-box;
    transition: background-color 120ms ease;
  }

  .list:hover::-webkit-scrollbar-thumb,
  .list:focus-within::-webkit-scrollbar-thumb {
    background-color: var(--vertical-bar-scroll-thumb-hover, rgba(95, 99, 104, 0.5));
  }
`;
